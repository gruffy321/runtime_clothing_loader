// Runtime Clothing Loader - UE 4.19.2

#include "RCLImportData.h"

namespace
{
	SIZE_T MeshAllocatedSize(const FRCLImportMesh& Mesh)
	{
		SIZE_T Size =
			Mesh.Name.GetAllocatedSize() +
			Mesh.SourceNodes.GetAllocatedSize() +
			Mesh.Positions.GetAllocatedSize() +
			Mesh.Normals.GetAllocatedSize() +
			Mesh.Tangents.GetAllocatedSize() +
			Mesh.UVs.GetAllocatedSize() +
			Mesh.Colors.GetAllocatedSize() +
			Mesh.Indices.GetAllocatedSize() +
			Mesh.Sections.GetAllocatedSize() +
			Mesh.Influences.GetAllocatedSize() +
			Mesh.InfluenceStart.GetAllocatedSize();

		for (const FString& Node : Mesh.SourceNodes)
		{
			Size += Node.GetAllocatedSize();
		}
		for (const TArray<FVector2D>& Channel : Mesh.UVs)
		{
			Size += Channel.GetAllocatedSize();
		}
		return Size;
	}
}

SIZE_T FRCLImportData::GetAllocatedSize() const
{
	SIZE_T Size =
		SourceFile.GetAllocatedSize() +
		Meshes.GetAllocatedSize() +
		Joints.GetAllocatedSize() +
		Materials.GetAllocatedSize() +
		Textures.GetAllocatedSize() +
		Warnings.GetAllocatedSize();

	for (const FRCLImportMesh& Mesh : Meshes)
	{
		Size += MeshAllocatedSize(Mesh);
	}
	for (const FRCLImportTexture& Texture : Textures)
	{
		Size += Texture.RelativePath.GetAllocatedSize() + Texture.EmbeddedBytes.GetAllocatedSize();
	}
	for (const FString& Warning : Warnings)
	{
		Size += Warning.GetAllocatedSize();
	}
	return Size;
}
