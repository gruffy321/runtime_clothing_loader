// Runtime Clothing Loader - UE 4.19.2
// FBX and OBJ parsing through ufbx. See RCLFbxParser.h for the contract.

#include "RCLFbxParser.h"
#include "RCLUfbx.h"
#include "RuntimeClothingLoader.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/Paths.h"
#include "Misc/Crc.h"

namespace RCLFbx
{
	/** Max UV channels kept per vertex (UE's MAX_TEXCOORDS is 4 for skeletal meshes). */
	static const int32 MaxUVChannels = 4;

	/** Cap on ufbx warnings copied into the report, so a broken file can't flood the UI. */
	static const int32 MaxCopiedWarnings = 20;

	FString ToFString(const ufbx_string& S)
	{
		// ufbx strings are UTF-8 and always null-terminated.
		return S.length > 0 ? FString(UTF8_TO_TCHAR(S.data)) : FString();
	}

	FVector ToFVector(const ufbx_vec3& V)
	{
		return FVector((float)V.x, (float)V.y, (float)V.z);
	}

	FTransform ToFTransform(const ufbx_matrix& M)
	{
		const ufbx_transform T = ufbx_matrix_to_transform(&M);
		FQuat Rotation((float)T.rotation.x, (float)T.rotation.y, (float)T.rotation.z, (float)T.rotation.w);
		Rotation.Normalize();
		return FTransform(Rotation, ToFVector(T.translation), ToFVector(T.scale));
	}

	/** Mirror across the XZ plane (negate Y): how UE's FBX importer reaches left-handed space. */
	FVector MirrorY(const FVector& V)
	{
		return FVector(V.X, -V.Y, V.Z);
	}

	FTransform MirrorY(const FTransform& T)
	{
		// Reflection S = diag(1,-1,1); S * R * S for a quaternion (x, y, z, w) is (-x, y, -z, w).
		const FQuat Q = T.GetRotation();
		return FTransform(FQuat(-Q.X, Q.Y, -Q.Z, Q.W), MirrorY(T.GetTranslation()), T.GetScale3D());
	}

	bool StartsWithAny(const FString& Name, const TCHAR* const* Prefixes, int32 NumPrefixes)
	{
		for (int32 Index = 0; Index < NumPrefixes; ++Index)
		{
			if (Name.StartsWith(Prefixes[Index], ESearchCase::IgnoreCase))
			{
				return true;
			}
		}
		return false;
	}

	bool ContainsName(const TArray<FString>& List, const FString& Name)
	{
		for (const FString& Entry : List)
		{
			if (Entry.Equals(Name, ESearchCase::IgnoreCase))
			{
				return true;
			}
		}
		return false;
	}

	/** Folder-restricted file opener: ufbx may only open files inside the source file's folder. */
	struct FOpenFileContext
	{
		FString AllowedDirectory; // Absolute, normalised, ends with '/'.
		FString RejectedPath;
	};

	bool OpenFileRestricted(void* User, ufbx_stream* Stream, const char* Path, size_t PathLength, const ufbx_open_file_info* Info)
	{
		FOpenFileContext& Context = *static_cast<FOpenFileContext*>(User);

		const FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Path), (int32)PathLength);
		FString Requested(Converted.Length(), Converted.Get());
		FString Full = FPaths::ConvertRelativePathToFull(Context.AllowedDirectory, Requested);
		FPaths::NormalizeFilename(Full);
		FPaths::CollapseRelativeDirectories(Full);

		if (!Full.StartsWith(Context.AllowedDirectory, ESearchCase::IgnoreCase))
		{
			Context.RejectedPath = Requested;
			return false;
		}
		return ufbx_default_open_file(nullptr, Stream, Path, PathLength, Info);
	}

	/** One triangle corner's attributes; equal corners share a render vertex. Memcmp-safe (zeroed). */
	struct FCorner
	{
		uint32 LogicalVertex;
		int32 MaterialIndex;
		FVector Position;
		FVector Normal;
		FVector2D UVs[MaxUVChannels];
		FColor Color;

		FCorner() { FMemory::Memzero(this, sizeof(FCorner)); }

		bool operator==(const FCorner& Other) const { return FMemory::Memcmp(this, &Other, sizeof(FCorner)) == 0; }
		friend uint32 GetTypeHash(const FCorner& Corner) { return FCrc::MemCrc32(&Corner, sizeof(FCorner)); }
	};

	/** Reorders the index buffer so each material has exactly one section, in material order. */
	void CompactSections(FRCLImportMesh& Mesh)
	{
		if (Mesh.Sections.Num() <= 1)
		{
			return;
		}

		TMap<int32, TArray<uint32>> IndicesByMaterial;
		for (const FRCLImportSection& Section : Mesh.Sections)
		{
			TArray<uint32>& Target = IndicesByMaterial.FindOrAdd(Section.MaterialIndex);
			const uint32 First = Section.FirstIndex;
			const uint32 Count = Section.NumTriangles * 3;
			for (uint32 Offset = 0; Offset < Count; ++Offset)
			{
				Target.Add(Mesh.Indices[First + Offset]);
			}
		}

		IndicesByMaterial.KeySort(TLess<int32>());

		TArray<uint32> NewIndices;
		NewIndices.Reserve(Mesh.Indices.Num());
		TArray<FRCLImportSection> NewSections;
		for (const TPair<int32, TArray<uint32>>& Pair : IndicesByMaterial)
		{
			FRCLImportSection Section;
			Section.MaterialIndex = Pair.Key;
			Section.FirstIndex = (uint32)NewIndices.Num();
			Section.NumTriangles = (uint32)Pair.Value.Num() / 3;
			NewIndices.Append(Pair.Value);
			NewSections.Add(Section);
		}

		Mesh.Indices = MoveTemp(NewIndices);
		Mesh.Sections = MoveTemp(NewSections);
	}

	/** Bone node of the topmost joint above Bone (stops below the scene root). */
	const ufbx_node* SkeletonRootOf(const ufbx_node* Bone)
	{
		const ufbx_node* Current = Bone;
		while (Current && Current->parent && !Current->parent->is_root)
		{
			Current = Current->parent;
		}
		return Current;
	}
}

// ------------------------------------------------------------------------------------------------

FRCLFbxParser::FSceneHandle::~FSceneHandle()
{
	if (Scene)
	{
		ufbx_free_scene(Scene);
		Scene = nullptr;
	}
}

ERCLSourceFormat FRCLFbxParser::DetectFormat(const FString& FilePath) const
{
	const FString Extension = FPaths::GetExtension(FilePath).ToLower();
	if (Extension == TEXT("fbx"))
	{
		return ERCLSourceFormat::FBX;
	}
	if (Extension == TEXT("obj"))
	{
		return ERCLSourceFormat::OBJ;
	}
	return ERCLSourceFormat::Unknown;
}

bool FRCLFbxParser::PreflightFile(const FString& FilePath, const FRCLParseSettings& Settings,
	ERCLFailureReason& OutReason, FString& OutError)
{
	const FString Extension = FPaths::GetExtension(FilePath).ToLower();
	if (Extension != TEXT("fbx") && Extension != TEXT("obj"))
	{
		OutReason = ERCLFailureReason::UnsupportedFormat;
		OutError = FString::Printf(TEXT("'%s' is not an FBX or OBJ file."), *FPaths::GetCleanFilename(FilePath));
		return false;
	}

	const int64 FileSize = IFileManager::Get().FileSize(*FilePath);
	if (FileSize < 0)
	{
		OutReason = ERCLFailureReason::FileNotFound;
		OutError = FString::Printf(TEXT("File not found: %s"), *FilePath);
		return false;
	}
	if (FileSize == 0)
	{
		OutReason = ERCLFailureReason::ParseError;
		OutError = FString::Printf(TEXT("'%s' is empty."), *FPaths::GetCleanFilename(FilePath));
		return false;
	}
	if (FileSize > Settings.MaxFileSizeBytes)
	{
		OutReason = ERCLFailureReason::FileTooLarge;
		OutError = FString::Printf(TEXT("'%s' is %.1f MB; the limit is %.1f MB."), *FPaths::GetCleanFilename(FilePath),
			FileSize / (1024.0 * 1024.0), Settings.MaxFileSizeBytes / (1024.0 * 1024.0));
		return false;
	}
	return true;
}

bool FRCLFbxParser::LoadScene(const FString& FilePath, const FRCLParseSettings& Settings, bool bGeometry,
	FSceneHandle& OutScene, ERCLFailureReason& OutReason, FString& OutError)
{
	FString FullPath = FPaths::ConvertRelativePathToFull(FilePath);
	FPaths::NormalizeFilename(FullPath);

	RCLFbx::FOpenFileContext OpenContext;
	OpenContext.AllowedDirectory = FPaths::GetPath(FullPath) / TEXT("");

	ufbx_load_opts Options = {};
	if (Settings.bConvertToUnrealSpace)
	{
		// Right-handed Z-up centimetres; the Y mirror to left-handed happens afterwards, as in UE's FBX importer.
		Options.target_axes = ufbx_axes_right_handed_z_up;
		Options.target_unit_meters = (ufbx_real)0.01;
		Options.space_conversion = UFBX_SPACE_CONVERSION_MODIFY_GEOMETRY;

		// OBJ stores no units or axes. Assume what DAZ Studio exports by default (and what UE's
		// own OBJ import assumes): Y-up, 1 unit = 1 cm. ImportScale corrects other exporters.
		Options.obj_axes = ufbx_axes_right_handed_y_up;
		Options.obj_unit_meters = (ufbx_real)0.01;
	}
	Options.generate_missing_normals = true;
	Options.ignore_animation = true;
	Options.ignore_embedded = !(bGeometry && Settings.bReadTextures);
	// External files: only an OBJ's .mtl is wanted (it holds the material names and colours).
	// FBX external files (geometry caches) are never loaded. Every open still goes through the
	// folder-restricted callback below.
	Options.load_external_files = FPaths::GetExtension(FilePath).ToLower() == TEXT("obj");

	// A missing (or refused) .mtl is a warning, not a failure: the mesh still loads with default materials.
	Options.ignore_missing_external_files = true;
	Options.node_depth_limit = 512;

	// Memory caps: a small hostile file must not expand into gigabytes.
	const size_t MemoryLimit = (size_t)FMath::Max<int64>(Settings.MaxFileSizeBytes * 4, 64ll * 1024 * 1024);
	Options.temp_allocator.memory_limit = MemoryLimit;
	Options.result_allocator.memory_limit = MemoryLimit;

	// The main file opens normally; anything else (an OBJ's .mtl) must be in the same folder.
	Options.open_main_file_with_default = true;
	Options.open_file_cb.fn = &RCLFbx::OpenFileRestricted;
	Options.open_file_cb.user = &OpenContext;

	ufbx_error Error = {};
	OutScene.Scene = ufbx_load_file(TCHAR_TO_UTF8(*FullPath), &Options, &Error);

	if (!OpenContext.RejectedPath.IsEmpty())
	{
		UE_LOG(LogRuntimeClothing, Warning, TEXT("'%s' referenced a file outside its folder, which was not opened: %s"),
			*FPaths::GetCleanFilename(FilePath), *OpenContext.RejectedPath);
	}

	if (!OutScene.Scene)
	{
		OutReason = ERCLFailureReason::ParseError;
		OutError = FString::Printf(TEXT("Could not read '%s': %s"), *FPaths::GetCleanFilename(FilePath),
			*RCLFbx::ToFString(Error.description));
		return false;
	}
	return true;
}

bool FRCLFbxParser::LooksLikeDazFigure(const ufbx_node& Node)
{
	static const TCHAR* const FigurePrefixes[] = { TEXT("Genesis3"), TEXT("Genesis8"), TEXT("Genesis9") };
	const int32 NumPrefixes = ARRAY_COUNT(FigurePrefixes);

	if (!Node.mesh || Node.mesh->skin_deformers.count == 0)
	{
		return false;
	}
	if (!RCLFbx::StartsWithAny(RCLFbx::ToFString(Node.name), FigurePrefixes, NumPrefixes))
	{
		return false;
	}

	const ufbx_skin_deformer* Skin = Node.mesh->skin_deformers.data[0];
	for (size_t ClusterIndex = 0; ClusterIndex < Skin->clusters.count; ++ClusterIndex)
	{
		const ufbx_node* Bone = Skin->clusters.data[ClusterIndex]->bone_node;
		if (Bone)
		{
			const ufbx_node* Root = RCLFbx::SkeletonRootOf(Bone);
			return Root && RCLFbx::StartsWithAny(RCLFbx::ToFString(Root->name), FigurePrefixes, NumPrefixes);
		}
	}
	return false;
}

void FRCLFbxParser::SelectMeshNodes(const ufbx_scene& Scene, const FRCLParseSettings& Settings,
	TArray<const ufbx_node*>& OutNodes, TArray<FString>& OutWarnings)
{
	TArray<const ufbx_node*> Candidates;
	int32 NumSkinned = 0;

	for (size_t NodeIndex = 0; NodeIndex < Scene.nodes.count; ++NodeIndex)
	{
		const ufbx_node* Node = Scene.nodes.data[NodeIndex];
		if (!Node->mesh || Node->mesh->num_triangles == 0)
		{
			continue;
		}

		const FString Name = RCLFbx::ToFString(Node->name);
		if (Settings.IncludeMeshNodes.Num() > 0 && !RCLFbx::ContainsName(Settings.IncludeMeshNodes, Name))
		{
			continue;
		}
		if (RCLFbx::ContainsName(Settings.ExcludeMeshNodes, Name))
		{
			continue;
		}
		if (!Node->visible)
		{
			OutWarnings.Add(FString::Printf(TEXT("Skipped hidden part '%s'."), *Name));
			continue;
		}

		Candidates.Add(Node);
		if (Node->mesh->skin_deformers.count > 0)
		{
			++NumSkinned;
		}
	}

	// A lone part is never treated as the DAZ figure: renamed garment roots look like the figure.
	const bool bSkipFigures = Settings.bExcludeDazFigureMeshes && NumSkinned > 1;

	for (const ufbx_node* Node : Candidates)
	{
		if (bSkipFigures && LooksLikeDazFigure(*Node))
		{
			OutWarnings.Add(FString::Printf(TEXT("Skipped '%s': looks like the DAZ figure the clothing was fitted on."),
				*RCLFbx::ToFString(Node->name)));
			continue;
		}
		OutNodes.Add(Node);
	}
}

void FRCLFbxParser::BuildJoints(const ufbx_scene& Scene, const TArray<const ufbx_node*>& MeshNodes,
	FRCLImportData& OutData, TMap<const ufbx_node*, int32>& OutJointOfNode)
{
	// Bones any selected mesh is skinned to, with their bind pose from the first cluster that names them.
	TMap<const ufbx_node*, const ufbx_skin_cluster*> ClusterOfBone;
	for (const ufbx_node* Node : MeshNodes)
	{
		for (size_t SkinIndex = 0; SkinIndex < Node->mesh->skin_deformers.count; ++SkinIndex)
		{
			const ufbx_skin_deformer* Skin = Node->mesh->skin_deformers.data[SkinIndex];
			for (size_t ClusterIndex = 0; ClusterIndex < Skin->clusters.count; ++ClusterIndex)
			{
				const ufbx_skin_cluster* Cluster = Skin->clusters.data[ClusterIndex];
				if (Cluster->bone_node && !ClusterOfBone.Contains(Cluster->bone_node))
				{
					ClusterOfBone.Add(Cluster->bone_node, Cluster);
				}
			}
		}
	}
	if (ClusterOfBone.Num() == 0)
	{
		return;
	}

	// Add every ancestor up to (not including) the scene root, so the hierarchy is complete.
	TArray<const ufbx_node*> JointNodes;
	TSet<const ufbx_node*> Seen;
	for (const TPair<const ufbx_node*, const ufbx_skin_cluster*>& Pair : ClusterOfBone)
	{
		for (const ufbx_node* Current = Pair.Key; Current && !Current->is_root; Current = Current->parent)
		{
			if (Seen.Contains(Current))
			{
				break;
			}
			Seen.Add(Current);
			JointNodes.Add(Current);
		}
	}

	// Parents before children: depth first, then file order for a stable result.
	JointNodes.Sort([](const ufbx_node& A, const ufbx_node& B)
	{
		return A.node_depth != B.node_depth ? A.node_depth < B.node_depth : A.typed_id < B.typed_id;
	});

	OutData.Joints.Reserve(JointNodes.Num());
	for (const ufbx_node* Node : JointNodes)
	{
		FRCLImportJoint Joint;
		Joint.Name = FName(*RCLFbx::ToFString(Node->name));

		const int32* ParentIndex = Node->parent ? OutJointOfNode.Find(Node->parent) : nullptr;
		Joint.ParentIndex = ParentIndex ? *ParentIndex : INDEX_NONE;

		const ufbx_skin_cluster* const* Cluster = ClusterOfBone.Find(Node);
		Joint.BindPoseComponentSpace = RCLFbx::ToFTransform(Cluster ? (*Cluster)->bind_to_world : Node->node_to_world);

		OutJointOfNode.Add(Node, OutData.Joints.Add(Joint));
	}
}

void FRCLFbxParser::ReadMaterials(const ufbx_scene& Scene, FRCLImportData& OutData,
	TMap<const ufbx_material*, int32>& OutMaterialIndexOf)
{
	TMap<const ufbx_texture*, int32> TextureIndexOf;

	auto AddTexture = [&](const ufbx_texture* Texture) -> int32
	{
		if (!Texture)
		{
			return INDEX_NONE;
		}
		// Layered or procedural textures: use the first file texture they reference.
		if (Texture->type != UFBX_TEXTURE_FILE && Texture->file_textures.count > 0)
		{
			Texture = Texture->file_textures.data[0];
		}
		if (const int32* Existing = TextureIndexOf.Find(Texture))
		{
			return *Existing;
		}

		FRCLImportTexture Imported;
		Imported.RelativePath = RCLFbx::ToFString(Texture->relative_filename);
		if (Imported.RelativePath.IsEmpty())
		{
			Imported.RelativePath = RCLFbx::ToFString(Texture->filename);
		}
		if (Texture->content.size > 0)
		{
			Imported.EmbeddedBytes.Append(static_cast<const uint8*>(Texture->content.data), (int32)Texture->content.size);
		}
		if (Imported.RelativePath.IsEmpty() && !Imported.IsEmbedded())
		{
			return INDEX_NONE;
		}

		const int32 Index = OutData.Textures.Add(MoveTemp(Imported));
		TextureIndexOf.Add(Texture, Index);
		return Index;
	};

	auto Pick = [](const ufbx_material_map& Pbr, const ufbx_material_map& Fbx) -> const ufbx_texture*
	{
		if (Pbr.texture && Pbr.texture_enabled)
		{
			return Pbr.texture;
		}
		return (Fbx.texture && Fbx.texture_enabled) ? Fbx.texture : nullptr;
	};

	for (size_t MaterialIndex = 0; MaterialIndex < Scene.materials.count; ++MaterialIndex)
	{
		const ufbx_material* Material = Scene.materials.data[MaterialIndex];

		FRCLImportMaterial Imported;
		Imported.Name = FName(*RCLFbx::ToFString(Material->name));

		if (Material->pbr.base_color.has_value)
		{
			const ufbx_vec4& C = Material->pbr.base_color.value_vec4;
			Imported.BaseColorFactor = FLinearColor((float)C.x, (float)C.y, (float)C.z, Material->pbr.base_color.value_components >= 4 ? (float)C.w : 1.0f);
		}

		using Slot = ERCLTextureSlot;
		Imported.TextureIndices[(int32)Slot::BaseColor] = AddTexture(Pick(Material->pbr.base_color, Material->fbx.diffuse_color));
		Imported.TextureIndices[(int32)Slot::Normal]    = AddTexture(Pick(Material->pbr.normal_map, Material->fbx.normal_map));
		Imported.TextureIndices[(int32)Slot::Specular]  = AddTexture(Pick(Material->pbr.specular_color, Material->fbx.specular_color));
		Imported.TextureIndices[(int32)Slot::Roughness] = AddTexture(Pick(Material->pbr.roughness, Material->pbr.roughness));
		Imported.TextureIndices[(int32)Slot::Opacity]   = AddTexture(Pick(Material->pbr.opacity, Material->fbx.transparency_factor));
		Imported.TextureIndices[(int32)Slot::Emissive]  = AddTexture(Pick(Material->pbr.emission_color, Material->fbx.emission_color));

		OutMaterialIndexOf.Add(Material, OutData.Materials.Add(Imported));
	}

	// Faces with no material use this one.
	FRCLImportMaterial Default;
	Default.Name = FName(TEXT("Default"));
	OutMaterialIndexOf.Add(nullptr, OutData.Materials.Add(Default));
}

bool FRCLFbxParser::AppendMeshNode(const ufbx_node& Node, const TMap<const ufbx_node*, int32>& JointOfNode,
	const TMap<const ufbx_material*, int32>& MaterialIndexOf, FRCLImportMesh& OutMesh, TArray<FString>& OutWarnings)
{
	using namespace RCLFbx;

	const ufbx_mesh& Mesh = *Node.mesh;
	const FString NodeName = ToFString(Node.name);

	// Skinned meshes are placed by their bind pose; static ones by the node's current transform.
	const ufbx_skin_deformer* Skin = Mesh.skin_deformers.count > 0 ? Mesh.skin_deformers.data[0] : nullptr;
	if (Mesh.skin_deformers.count > 1)
	{
		OutWarnings.Add(FString::Printf(TEXT("'%s' has %d skin deformers; only the first is used."), *NodeName, (int32)Mesh.skin_deformers.count));
	}

	ufbx_matrix GeometryToWorld = Node.geometry_to_world;
	if (Skin && Skin->clusters.count > 0)
	{
		const ufbx_skin_cluster* Cluster = Skin->clusters.data[0];
		GeometryToWorld = ufbx_matrix_mul(&Cluster->bind_to_world, &Cluster->geometry_to_bone);
	}
	const ufbx_matrix NormalToWorld = ufbx_matrix_for_normals(&GeometryToWorld);

	// A mirrored node (odd negative scale) needs its winding reversed, as in UE's importer.
	const bool bReverseWinding = ufbx_matrix_determinant(&GeometryToWorld) < 0;

	if (Mesh.num_point_faces + Mesh.num_line_faces > 0)
	{
		OutWarnings.Add(FString::Printf(TEXT("'%s': skipped %d point and line faces."), *NodeName,
			(int32)(Mesh.num_point_faces + Mesh.num_line_faces)));
	}

	const int32 NumUVSets = FMath::Min((int32)Mesh.uv_sets.count, MaxUVChannels);
	const bool bHasColors = Mesh.vertex_color.exists;
	const bool bHasNormals = Mesh.vertex_normal.exists;

	// Make sure every existing vertex has an influence range before appending (merged files can mix
	// skinned and static parts).
	const int32 BaseVertex = OutMesh.Positions.Num();
	if (OutMesh.InfluenceStart.Num() == 0)
	{
		OutMesh.InfluenceStart.Add(0);
	}

	while (OutMesh.UVs.Num() < NumUVSets)
	{
		TArray<FVector2D> Channel;
		Channel.SetNumZeroed(OutMesh.Positions.Num());
		OutMesh.UVs.Add(MoveTemp(Channel));
	}

	TMap<FCorner, uint32> VertexOfCorner;
	TMap<int32, TArray<uint32>> TrianglesByMaterial;

	TArray<uint32> TriangleCorners;
	TriangleCorners.SetNumUninitialized((int32)Mesh.max_face_triangles * 3);

	for (size_t FaceIndex = 0; FaceIndex < Mesh.num_faces; ++FaceIndex)
	{
		const ufbx_face Face = Mesh.faces.data[FaceIndex];
		if (Face.num_indices < 3)
		{
			continue;
		}

		const uint32 NumTriangles = ufbx_triangulate_face(TriangleCorners.GetData(), TriangleCorners.Num(), &Mesh, Face);

		int32 MaterialIndex = *MaterialIndexOf.Find(nullptr);
		if (Mesh.face_material.count > FaceIndex)
		{
			const uint32 Slot = Mesh.face_material.data[FaceIndex];
			const ufbx_material* Material =
				(Slot < Node.materials.count) ? Node.materials.data[Slot] :
				(Slot < Mesh.materials.count) ? Mesh.materials.data[Slot] : nullptr;
			if (const int32* Found = MaterialIndexOf.Find(Material))
			{
				MaterialIndex = *Found;
			}
		}
		TArray<uint32>& Triangles = TrianglesByMaterial.FindOrAdd(MaterialIndex);

		for (uint32 Triangle = 0; Triangle < NumTriangles; ++Triangle)
		{
			uint32 Vertices[3];
			for (int32 Corner = 0; Corner < 3; ++Corner)
			{
				const uint32 Index = TriangleCorners[Triangle * 3 + Corner];

				FCorner Key;
				Key.LogicalVertex = Mesh.vertex_indices.data[Index];
				Key.MaterialIndex = MaterialIndex;
				Key.Position = ToFVector(ufbx_transform_position(&GeometryToWorld, ufbx_get_vertex_vec3(&Mesh.vertex_position, Index)));
				Key.Normal = bHasNormals
					? ToFVector(ufbx_transform_direction(&NormalToWorld, ufbx_get_vertex_vec3(&Mesh.vertex_normal, Index))).GetSafeNormal()
					: FVector::UpVector;
				if (Key.Normal.IsZero())
				{
					Key.Normal = FVector::UpVector; // degenerate normal in the file
				}
				for (int32 Channel = 0; Channel < NumUVSets; ++Channel)
				{
					const ufbx_vec2 UV = ufbx_get_vertex_vec2(&Mesh.uv_sets.data[Channel].vertex_uv, Index);
					Key.UVs[Channel] = FVector2D((float)UV.x, 1.0f - (float)UV.y); // FBX V-up to UE V-down
				}
				if (bHasColors)
				{
					const ufbx_vec4 C = ufbx_get_vertex_vec4(&Mesh.vertex_color, Index);
					Key.Color = FLinearColor((float)C.x, (float)C.y, (float)C.z, (float)C.w).ToFColor(false);
				}
				else
				{
					Key.Color = FColor::White;
				}

				if (const uint32* Existing = VertexOfCorner.Find(Key))
				{
					Vertices[Corner] = *Existing;
					continue;
				}

				const uint32 NewVertex = (uint32)OutMesh.Positions.Add(Key.Position);
				OutMesh.Normals.Add(Key.Normal);
				for (int32 Channel = 0; Channel < OutMesh.UVs.Num(); ++Channel)
				{
					OutMesh.UVs[Channel].Add(Channel < NumUVSets ? Key.UVs[Channel] : FVector2D::ZeroVector);
				}
				OutMesh.Colors.Add(Key.Color);

				if (Skin && Key.LogicalVertex < Skin->vertices.count)
				{
					const ufbx_skin_vertex& SkinVertex = Skin->vertices.data[Key.LogicalVertex];
					for (uint32 WeightIndex = 0; WeightIndex < SkinVertex.num_weights; ++WeightIndex)
					{
						const ufbx_skin_weight& Weight = Skin->weights.data[SkinVertex.weight_begin + WeightIndex];
						const ufbx_node* Bone = Skin->clusters.data[Weight.cluster_index]->bone_node;
						const int32* Joint = Bone ? JointOfNode.Find(Bone) : nullptr;
						if (Joint && Weight.weight > 0)
						{
							FRCLInfluence Influence;
							Influence.JointIndex = *Joint;
							Influence.Weight = (float)Weight.weight;
							OutMesh.Influences.Add(Influence);
						}
					}
				}
				OutMesh.InfluenceStart.Add((uint32)OutMesh.Influences.Num());

				VertexOfCorner.Add(Key, NewVertex);
				Vertices[Corner] = NewVertex;
			}

			if (Vertices[0] == Vertices[1] || Vertices[1] == Vertices[2] || Vertices[0] == Vertices[2])
			{
				continue; // degenerate after de-duplication
			}

			if (bReverseWinding)
			{
				Triangles.Add(Vertices[0]); Triangles.Add(Vertices[2]); Triangles.Add(Vertices[1]);
			}
			else
			{
				Triangles.Add(Vertices[0]); Triangles.Add(Vertices[1]); Triangles.Add(Vertices[2]);
			}
		}
	}

	int32 NumTrianglesAdded = 0;
	for (const TPair<int32, TArray<uint32>>& Pair : TrianglesByMaterial)
	{
		if (Pair.Value.Num() == 0)
		{
			continue;
		}
		FRCLImportSection Section;
		Section.MaterialIndex = Pair.Key;
		Section.FirstIndex = (uint32)OutMesh.Indices.Num();
		Section.NumTriangles = (uint32)Pair.Value.Num() / 3;
		OutMesh.Indices.Append(Pair.Value);
		OutMesh.Sections.Add(Section);
		NumTrianglesAdded += Section.NumTriangles;
	}

	OutMesh.SourceNodes.Add(NodeName);

	if (NumTrianglesAdded == 0)
	{
		OutWarnings.Add(FString::Printf(TEXT("'%s' produced no triangles."), *NodeName));
		return false;
	}

	UE_LOG(LogRuntimeClothing, Verbose, TEXT("Part '%s': %d vertices, %d triangles%s."), *NodeName,
		OutMesh.Positions.Num() - BaseVertex, NumTrianglesAdded, Skin ? TEXT(", skinned") : TEXT(""));
	return true;
}

void FRCLFbxParser::ConvertToUnrealHandedness(FRCLImportData& Data)
{
	for (FRCLImportMesh& Mesh : Data.Meshes)
	{
		for (FVector& Position : Mesh.Positions)
		{
			Position = RCLFbx::MirrorY(Position);
		}
		for (FVector& Normal : Mesh.Normals)
		{
			Normal = RCLFbx::MirrorY(Normal);
		}
		// Winding is kept: a Y mirror with unchanged index order is what UE's FBX importer produces.
	}
	for (FRCLImportJoint& Joint : Data.Joints)
	{
		Joint.BindPoseComponentSpace = RCLFbx::MirrorY(Joint.BindPoseComponentSpace);
	}
}

bool FRCLFbxParser::CheckLimits(const FRCLImportData& Data, const FRCLParseSettings& Settings,
	ERCLFailureReason& OutReason, FString& OutError)
{
	for (const FRCLImportMesh& Mesh : Data.Meshes)
	{
		if (Mesh.NumVertices() > Settings.MaxVertices)
		{
			OutReason = ERCLFailureReason::TooManyVertices;
			OutError = FString::Printf(TEXT("'%s' has %d vertices; the limit is %d."), *Mesh.Name, Mesh.NumVertices(), Settings.MaxVertices);
			return false;
		}
	}
	if (Data.Joints.Num() > Settings.MaxBones)
	{
		OutReason = ERCLFailureReason::TooManyBones;
		OutError = FString::Printf(TEXT("The file uses %d bones; the limit is %d."), Data.Joints.Num(), Settings.MaxBones);
		return false;
	}
	return true;
}

bool FRCLFbxParser::Inspect(const FString& FilePath, const FRCLParseSettings& Settings,
	TArray<FRCLMeshNodeInfo>& OutNodes, ERCLFailureReason& OutReason, FString& OutError)
{
	OutNodes.Reset();
	OutReason = ERCLFailureReason::None;

	if (!PreflightFile(FilePath, Settings, OutReason, OutError))
	{
		return false;
	}

	FSceneHandle Scene;
	if (!LoadScene(FilePath, Settings, /*bGeometry*/ false, Scene, OutReason, OutError))
	{
		return false;
	}

	for (size_t NodeIndex = 0; NodeIndex < Scene.Scene->nodes.count; ++NodeIndex)
	{
		const ufbx_node* Node = Scene.Scene->nodes.data[NodeIndex];
		if (!Node->mesh || Node->mesh->num_triangles == 0)
		{
			continue;
		}

		FRCLMeshNodeInfo Info;
		Info.Name = RCLFbx::ToFString(Node->name);
		Info.NumVertices = (int32)Node->mesh->num_vertices;
		Info.NumTriangles = (int32)Node->mesh->num_triangles;
		Info.NumMaterials = (int32)FMath::Max(Node->materials.count, Node->mesh->materials.count);
		Info.bIsSkinned = Node->mesh->skin_deformers.count > 0;
		Info.bLooksLikeDazFigure = LooksLikeDazFigure(*Node);
		OutNodes.Add(Info);
	}

	if (OutNodes.Num() == 0)
	{
		OutReason = ERCLFailureReason::NoMeshInFile;
		OutError = FString::Printf(TEXT("'%s' contains no meshes."), *FPaths::GetCleanFilename(FilePath));
		return false;
	}
	return true;
}

bool FRCLFbxParser::Parse(const FString& FilePath, const FRCLParseSettings& Settings,
	FRCLImportData& OutData, ERCLFailureReason& OutReason, FString& OutError)
{
	const double StartTime = FPlatformTime::Seconds();
	OutData = FRCLImportData();
	OutReason = ERCLFailureReason::None;

	if (!PreflightFile(FilePath, Settings, OutReason, OutError))
	{
		return false;
	}

	FSceneHandle Scene;
	if (!LoadScene(FilePath, Settings, /*bGeometry*/ true, Scene, OutReason, OutError))
	{
		return false;
	}

	FRCLImportData Data;
	Data.SourceFile = FilePath;
	Data.Format = DetectFormat(FilePath);

	const ufbx_scene& UScene = *Scene.Scene;
	// ufbx can repeat the same warning many times (e.g. DAZ exports with non-UTF-8 labels):
	// collapse identical ones into a single line with a count, in first-seen order.
	{
		TArray<FString> Unique;
		TMap<FString, int32> Counts;
		for (size_t WarningIndex = 0; WarningIndex < UScene.metadata.warnings.count; ++WarningIndex)
		{
			const FString Text = RCLFbx::ToFString(UScene.metadata.warnings.data[WarningIndex].description);
			int32& Count = Counts.FindOrAdd(Text);
			if (Count++ == 0 && Unique.Num() < RCLFbx::MaxCopiedWarnings)
			{
				Unique.Add(Text);
			}
		}
		for (const FString& Text : Unique)
		{
			const int32 Count = Counts.FindChecked(Text);
			Data.Warnings.Add(Count > 1 ? FString::Printf(TEXT("%s (x%d)"), *Text, Count) : Text);
		}
	}

	TArray<const ufbx_node*> MeshNodes;
	SelectMeshNodes(UScene, Settings, MeshNodes, Data.Warnings);
	if (MeshNodes.Num() == 0)
	{
		OutReason = ERCLFailureReason::NoMeshInFile;
		OutError = FString::Printf(TEXT("'%s' contains no meshes to load."), *FPaths::GetCleanFilename(FilePath));
		return false;
	}

	TMap<const ufbx_node*, int32> JointOfNode;
	BuildJoints(UScene, MeshNodes, Data, JointOfNode);

	TMap<const ufbx_material*, int32> MaterialIndexOf;
	ReadMaterials(UScene, Data, MaterialIndexOf);

	if (Settings.Grouping == ERCLMeshGrouping::MergeAll)
	{
		FRCLImportMesh Merged;
		Merged.Name = FPaths::GetBaseFilename(FilePath);
		for (const ufbx_node* Node : MeshNodes)
		{
			AppendMeshNode(*Node, JointOfNode, MaterialIndexOf, Merged, Data.Warnings);
		}
		if (Merged.Indices.Num() > 0)
		{
			Data.Meshes.Add(MoveTemp(Merged));
		}
	}
	else
	{
		for (const ufbx_node* Node : MeshNodes)
		{
			FRCLImportMesh Part;
			Part.Name = RCLFbx::ToFString(Node->name);
			if (AppendMeshNode(*Node, JointOfNode, MaterialIndexOf, Part, Data.Warnings))
			{
				Data.Meshes.Add(MoveTemp(Part));
			}
		}
	}

	if (Data.Meshes.Num() == 0)
	{
		OutReason = ERCLFailureReason::NoMeshInFile;
		OutError = FString::Printf(TEXT("'%s' contains no usable triangles."), *FPaths::GetCleanFilename(FilePath));
		return false;
	}

	for (FRCLImportMesh& Mesh : Data.Meshes)
	{
		RCLFbx::CompactSections(Mesh);

		// Unskinned garments carry no influence arrays at all (see RCLImportData.h).
		if (Mesh.Influences.Num() == 0)
		{
			Mesh.InfluenceStart.Reset();
		}

		if (!FMath::IsNearlyEqual(Settings.ImportScale, 1.0f))
		{
			for (FVector& Position : Mesh.Positions)
			{
				Position *= Settings.ImportScale;
			}
		}
	}
	if (!FMath::IsNearlyEqual(Settings.ImportScale, 1.0f))
	{
		for (FRCLImportJoint& Joint : Data.Joints)
		{
			Joint.BindPoseComponentSpace.ScaleTranslation(Settings.ImportScale);
		}
	}

	for (FRCLImportJoint& Joint : Data.Joints)
	{
		Joint.bInfluencesVertices = false;
	}
	for (const FRCLImportMesh& Mesh : Data.Meshes)
	{
		for (const FRCLInfluence& Influence : Mesh.Influences)
		{
			Data.Joints[Influence.JointIndex].bInfluencesVertices = true;
		}
	}

	if (Settings.bConvertToUnrealSpace)
	{
		ConvertToUnrealHandedness(Data);
	}

	if (!CheckLimits(Data, Settings, OutReason, OutError))
	{
		return false;
	}

	OutData = MoveTemp(Data);

	UE_LOG(LogRuntimeClothing, Log, TEXT("Parsed '%s': %d garment(s), %d joint(s), %d material(s), %d texture(s) in %.1f ms."),
		*FPaths::GetCleanFilename(FilePath), OutData.Meshes.Num(), OutData.Joints.Num(), OutData.Materials.Num(),
		OutData.Textures.Num(), (FPlatformTime::Seconds() - StartTime) * 1000.0);
	return true;
}
