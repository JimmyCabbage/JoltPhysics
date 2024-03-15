// Jolt Physics Library (https://github.com/jrouwe/JoltPhysics)
// SPDX-FileCopyrightText: 2023 Jorrit Rouwe
// SPDX-License-Identifier: MIT

#include <Jolt/Jolt.h>

#include <Jolt/Physics/SoftBody/SoftBodySharedSettings.h>
#include <Jolt/Physics/SoftBody/SoftBodyUpdateContext.h>
#include <Jolt/ObjectStream/TypeDeclarations.h>
#include <Jolt/Core/StreamIn.h>
#include <Jolt/Core/StreamOut.h>
#include <Jolt/Core/QuickSort.h>
#include <Jolt/Core/UnorderedMap.h>
#include <Jolt/Core/UnorderedSet.h>

JPH_NAMESPACE_BEGIN

JPH_IMPLEMENT_SERIALIZABLE_NON_VIRTUAL(SoftBodySharedSettings::Vertex)
{
	JPH_ADD_ATTRIBUTE(SoftBodySharedSettings::Vertex, mPosition)
	JPH_ADD_ATTRIBUTE(SoftBodySharedSettings::Vertex, mVelocity)
	JPH_ADD_ATTRIBUTE(SoftBodySharedSettings::Vertex, mInvMass)
}

JPH_IMPLEMENT_SERIALIZABLE_NON_VIRTUAL(SoftBodySharedSettings::Face)
{
	JPH_ADD_ATTRIBUTE(SoftBodySharedSettings::Face, mVertex)
	JPH_ADD_ATTRIBUTE(SoftBodySharedSettings::Face, mMaterialIndex)
}

JPH_IMPLEMENT_SERIALIZABLE_NON_VIRTUAL(SoftBodySharedSettings::Edge)
{
	JPH_ADD_ATTRIBUTE(SoftBodySharedSettings::Edge, mVertex)
	JPH_ADD_ATTRIBUTE(SoftBodySharedSettings::Edge, mRestLength)
	JPH_ADD_ATTRIBUTE(SoftBodySharedSettings::Edge, mStiffness)
}

JPH_IMPLEMENT_SERIALIZABLE_NON_VIRTUAL(SoftBodySharedSettings::Volume)
{
	JPH_ADD_ATTRIBUTE(SoftBodySharedSettings::Volume, mVertex)
	JPH_ADD_ATTRIBUTE(SoftBodySharedSettings::Volume, mSixRestVolume)
	JPH_ADD_ATTRIBUTE(SoftBodySharedSettings::Volume, mStiffness)
}

JPH_IMPLEMENT_SERIALIZABLE_NON_VIRTUAL(SoftBodySharedSettings::InvBind)
{
	JPH_ADD_ATTRIBUTE(SoftBodySharedSettings::InvBind, mJointIndex)
	JPH_ADD_ATTRIBUTE(SoftBodySharedSettings::InvBind, mInvBind)
}

JPH_IMPLEMENT_SERIALIZABLE_NON_VIRTUAL(SoftBodySharedSettings::SkinWeight)
{
	JPH_ADD_ATTRIBUTE(SoftBodySharedSettings::SkinWeight, mInvBindIndex)
	JPH_ADD_ATTRIBUTE(SoftBodySharedSettings::SkinWeight, mWeight)
}

JPH_IMPLEMENT_SERIALIZABLE_NON_VIRTUAL(SoftBodySharedSettings::Skinned)
{
	JPH_ADD_ATTRIBUTE(SoftBodySharedSettings::Skinned, mVertex)
	JPH_ADD_ATTRIBUTE(SoftBodySharedSettings::Skinned, mWeights)
	JPH_ADD_ATTRIBUTE(SoftBodySharedSettings::Skinned, mMaxDistance)
	JPH_ADD_ATTRIBUTE(SoftBodySharedSettings::Skinned, mBackStopDistance)
	JPH_ADD_ATTRIBUTE(SoftBodySharedSettings::Skinned, mBackStopRadius)
}

JPH_IMPLEMENT_SERIALIZABLE_NON_VIRTUAL(SoftBodySharedSettings::LRA)
{
	JPH_ADD_ATTRIBUTE(SoftBodySharedSettings::LRA, mVertex)
	JPH_ADD_ATTRIBUTE(SoftBodySharedSettings::LRA, mMaxDistance)
}

JPH_IMPLEMENT_SERIALIZABLE_NON_VIRTUAL(SoftBodySharedSettings)
{
	JPH_ADD_ATTRIBUTE(SoftBodySharedSettings, mVertices)
	JPH_ADD_ATTRIBUTE(SoftBodySharedSettings, mFaces)
	JPH_ADD_ATTRIBUTE(SoftBodySharedSettings, mEdgeConstraints)
	JPH_ADD_ATTRIBUTE(SoftBodySharedSettings, mEdgeGroupEndIndices)
	JPH_ADD_ATTRIBUTE(SoftBodySharedSettings, mVolumeConstraints)
	JPH_ADD_ATTRIBUTE(SoftBodySharedSettings, mSkinnedConstraints)
	JPH_ADD_ATTRIBUTE(SoftBodySharedSettings, mInvBindMatrices)
	JPH_ADD_ATTRIBUTE(SoftBodySharedSettings, mLRAConstraints)
	JPH_ADD_ATTRIBUTE(SoftBodySharedSettings, mMaterials)
	JPH_ADD_ATTRIBUTE(SoftBodySharedSettings, mVertexRadius)
}

void SoftBodySharedSettings::CreateEdges(float inStiffness, float inAngleTolerance)
{
	struct EdgeHelper
	{
		uint32	mVertex[2];
		uint32	mEdgeIdx;
	};

	// Create list of all edges
	Array<EdgeHelper> edges;
	edges.reserve(mFaces.size() * 3);
	for (const Face &f : mFaces)
		for (int i = 0; i < 3; ++i)
		{
			uint32 v0 = f.mVertex[i];
			uint32 v1 = f.mVertex[(i + 1) % 3];

			EdgeHelper e;
			e.mVertex[0] = min(v0, v1);
			e.mVertex[1] = max(v0, v1);
			e.mEdgeIdx = uint32(&f - mFaces.data()) * 3 + i;
			edges.push_back(e);
		}

	// Sort the edges
	QuickSort(edges.begin(), edges.end(), [](const EdgeHelper &inLHS, const EdgeHelper &inRHS) { return inLHS.mVertex[0] < inRHS.mVertex[0] || (inLHS.mVertex[0] == inRHS.mVertex[0] && inLHS.mVertex[1] < inRHS.mVertex[1]); });

	// Only add edges if one of the vertices is movable
	auto add_edge = [this](uint32 inVtx1, uint32 inVtx2, float inStiffnessParam) {
		if (mVertices[inVtx1].mInvMass > 0.0f || mVertices[inVtx2].mInvMass > 0.0f)
		{
			Edge temp_edge;
			temp_edge.mVertex[0] = inVtx1;
			temp_edge.mVertex[1] = inVtx2;
			temp_edge.mStiffness = inStiffnessParam;
			temp_edge.mRestLength = (Vec3(mVertices[inVtx2].mPosition) - Vec3(mVertices[inVtx1].mPosition)).Length();
			JPH_ASSERT(temp_edge.mRestLength > 0.0f);
			mEdgeConstraints.push_back(temp_edge);
		}
	};

	// Create the constraints
	float sq_sin_tolerance = Square(Sin(inAngleTolerance));
	float sq_cos_tolerance = Square(Cos(inAngleTolerance));
	mEdgeConstraints.clear();
	mEdgeConstraints.reserve(edges.size());
	for (Array<EdgeHelper>::size_type i = 0; i < edges.size(); ++i)
	{
		const EdgeHelper &e0 = edges[i];

		// Create a regular edge constraint
		add_edge(e0.mVertex[0], e0.mVertex[1], inStiffness);

		// Test if there are any shared edges
		for (Array<EdgeHelper>::size_type j = i + 1; j < edges.size(); ++j)
		{
			const EdgeHelper &e1 = edges[j];
			if (e0.mVertex[0] == e1.mVertex[0] && e0.mVertex[1] == e1.mVertex[1])
			{
				// Faces should be roughly in a plane
				const Face &f0 = mFaces[e0.mEdgeIdx / 3];
				const Face &f1 = mFaces[e1.mEdgeIdx / 3];
				Vec3 n0 = (Vec3(mVertices[f0.mVertex[2]].mPosition) - Vec3(mVertices[f0.mVertex[0]].mPosition)).Cross(Vec3(mVertices[f0.mVertex[1]].mPosition) - Vec3(mVertices[f0.mVertex[0]].mPosition));
				Vec3 n1 = (Vec3(mVertices[f1.mVertex[2]].mPosition) - Vec3(mVertices[f1.mVertex[0]].mPosition)).Cross(Vec3(mVertices[f1.mVertex[1]].mPosition) - Vec3(mVertices[f1.mVertex[0]].mPosition));
				if (Square(n0.Dot(n1)) > sq_cos_tolerance * n0.LengthSq() * n1.LengthSq())
				{
					// Get opposing vertices
					uint32 v0 = f0.mVertex[(e0.mEdgeIdx + 2) % 3];
					uint32 v1 = f1.mVertex[(e1.mEdgeIdx + 2) % 3];

					// Faces should approximately form a quad
					Vec3 e0_dir = Vec3(mVertices[v0].mPosition) - Vec3(mVertices[e0.mVertex[0]].mPosition);
					Vec3 e1_dir = Vec3(mVertices[v1].mPosition) - Vec3(mVertices[e0.mVertex[0]].mPosition);
					if (Square(e0_dir.Dot(e1_dir)) < sq_sin_tolerance * e0_dir.LengthSq() * e1_dir.LengthSq())
					{
						// Shear constraint
						add_edge(min(v0, v1), max(v0, v1), inStiffness);
					}
				}
			}
			else
			{
				// Start iterating from the first non-shared edge
				i = j - 1;
				break;
			}
		}
	}
	mEdgeConstraints.shrink_to_fit();
}

void SoftBodySharedSettings::CalculateEdgeLengths()
{
	for (Edge &e : mEdgeConstraints)
	{
		e.mRestLength = (Vec3(mVertices[e.mVertex[1]].mPosition) - Vec3(mVertices[e.mVertex[0]].mPosition)).Length();
		JPH_ASSERT(e.mRestLength > 0.0f);
	}
}

void SoftBodySharedSettings::CalculateLRALengths()
{
	for (LRA &l : mLRAConstraints)
	{
		l.mMaxDistance = (Vec3(mVertices[l.mVertex[1]].mPosition) - Vec3(mVertices[l.mVertex[0]].mPosition)).Length();
		JPH_ASSERT(l.mMaxDistance > 0.0f);
	}
}

void SoftBodySharedSettings::CalculateVolumeConstraintVolumes()
{
	for (Volume &v : mVolumeConstraints)
	{
		Vec3 x1(mVertices[v.mVertex[0]].mPosition);
		Vec3 x2(mVertices[v.mVertex[1]].mPosition);
		Vec3 x3(mVertices[v.mVertex[2]].mPosition);
		Vec3 x4(mVertices[v.mVertex[3]].mPosition);

		Vec3 x1x2 = x2 - x1;
		Vec3 x1x3 = x3 - x1;
		Vec3 x1x4 = x4 - x1;

		v.mSixRestVolume = abs(x1x2.Cross(x1x3).Dot(x1x4));
	}
}

void SoftBodySharedSettings::CalculateSkinnedConstraintNormals()
{
	// Clear any previous results
	mSkinnedConstraintNormals.clear();

	// If there are no skinned constraints, we're done
	if (mSkinnedConstraints.empty())
		return;

	// First collect all vertices that are skinned
	UnorderedSet<uint32> skinned_vertices;
	skinned_vertices.reserve(mSkinnedConstraints.size());
	for (const Skinned &s : mSkinnedConstraints)
		skinned_vertices.insert(s.mVertex);

	// Now collect all faces that connect only to skinned vertices
	UnorderedMap<uint32, UnorderedSet<uint32>> connected_faces;
	connected_faces.reserve(mVertices.size());
	for (const Face &f : mFaces)
	{
		// Must connect to only skinned vertices
		bool valid = true;
		for (uint32 v : f.mVertex)
			valid &= skinned_vertices.find(v) != skinned_vertices.end();
		if (!valid)
			continue;

		// Store faces that connect to vertices
		for (uint32 v : f.mVertex)
			connected_faces[v].insert(uint32(&f - mFaces.data()));
	}

	// Populate the list of connecting faces per skinned vertex
	mSkinnedConstraintNormals.reserve(mFaces.size());
	for (Skinned &s : mSkinnedConstraints)
	{
		uint32 start = uint32(mSkinnedConstraintNormals.size());
		JPH_ASSERT((start >> 24) == 0);
		const UnorderedSet<uint32> &faces = connected_faces[s.mVertex];
		uint32 num = uint32(faces.size());
		JPH_ASSERT(num < 256);
		mSkinnedConstraintNormals.insert(mSkinnedConstraintNormals.end(), faces.begin(), faces.end());
		QuickSort(mSkinnedConstraintNormals.begin() + start, mSkinnedConstraintNormals.begin() + start + num);
		s.mNormalInfo = start + (num << 24);
	}
	mSkinnedConstraintNormals.shrink_to_fit();
}

void SoftBodySharedSettings::Optimize(OptimizationResults &outResults)
{
	const uint cMaxNumGroups = 32;
	const uint cNonParallelGroupIdx = cMaxNumGroups - 1;
	const uint cMinimumSize = 2 * SoftBodyUpdateContext::cEdgeConstraintBatch; // There should be at least 2 batches, otherwise there's no point in parallelizing

	// Assign edges to non-overlapping groups
	Array<uint32> masks;
	masks.resize(mVertices.size(), 0);
	Array<uint> edge_groups[cMaxNumGroups];
	for (const Edge &e : mEdgeConstraints)
	{
		uint32 &mask1 = masks[e.mVertex[0]];
		uint32 &mask2 = masks[e.mVertex[1]];
		uint group = min(CountTrailingZeros((~mask1) & (~mask2)), cNonParallelGroupIdx);
		uint32 mask = uint32(1U << group);
		mask1 |= mask;
		mask2 |= mask;
		edge_groups[group].push_back(uint(&e - mEdgeConstraints.data()));
	}

	// Merge groups that are too small into the non-parallel group
	for (uint i = 0; i < cNonParallelGroupIdx; ++i)
		if (edge_groups[i].size() < cMinimumSize)
		{
			edge_groups[cNonParallelGroupIdx].insert(edge_groups[cNonParallelGroupIdx].end(), edge_groups[i].begin(), edge_groups[i].end());
			edge_groups[i].clear();
		}

	// Order the edges so that the ones with the smallest index go first (hoping to get better cache locality when we process the edges).
	// Note we could also re-order the vertices but that would be much more of a burden to the end user
	for (Array<uint> &group : edge_groups)
		QuickSort(group.begin(), group.end(), [this](uint inLHS, uint inRHS)
			{
				const Edge &e1 = mEdgeConstraints[inLHS];
				const Edge &e2 = mEdgeConstraints[inRHS];
				return min(e1.mVertex[0], e1.mVertex[1]) < min(e2.mVertex[0], e2.mVertex[1]);
			});

	// Assign the edges to groups and reorder them
	Array<Edge> temp_edges;
	temp_edges.swap(mEdgeConstraints);
	mEdgeConstraints.reserve(temp_edges.size());
	for (const Array<uint> &group : edge_groups)
		if (!group.empty())
		{
			for (uint idx : group)
			{
				mEdgeConstraints.push_back(temp_edges[idx]);
				outResults.mEdgeRemap.push_back(idx);
			}
			mEdgeGroupEndIndices.push_back((uint)mEdgeConstraints.size());
		}

	// If there is no non-parallel group then add an empty group at the end
	if (edge_groups[cNonParallelGroupIdx].empty())
		mEdgeGroupEndIndices.push_back((uint)mEdgeConstraints.size());
}

Ref<SoftBodySharedSettings> SoftBodySharedSettings::Clone() const
{
	Ref<SoftBodySharedSettings> clone = new SoftBodySharedSettings;
	clone->mVertices = mVertices;
	clone->mFaces = mFaces;
	clone->mEdgeConstraints = mEdgeConstraints;
	clone->mEdgeGroupEndIndices = mEdgeGroupEndIndices;
	clone->mVolumeConstraints = mVolumeConstraints;
	clone->mSkinnedConstraints = mSkinnedConstraints;
	clone->mSkinnedConstraintNormals = mSkinnedConstraintNormals;
	clone->mInvBindMatrices = mInvBindMatrices;
	clone->mLRAConstraints = mLRAConstraints;
	clone->mMaterials = mMaterials;
	clone->mVertexRadius = mVertexRadius;
	return clone;
}

void SoftBodySharedSettings::SaveBinaryState(StreamOut &inStream) const
{
	inStream.Write(mVertices);
	inStream.Write(mFaces);
	inStream.Write(mEdgeConstraints);
	inStream.Write(mEdgeGroupEndIndices);
	inStream.Write(mVolumeConstraints);
	inStream.Write(mSkinnedConstraints);
	inStream.Write(mSkinnedConstraintNormals);
	inStream.Write(mLRAConstraints);
	inStream.Write(mVertexRadius);

	// Can't write mInvBindMatrices directly because the class contains padding
	inStream.Write(mInvBindMatrices, [](const InvBind &inElement, StreamOut &inS) {
		inS.Write(inElement.mJointIndex);
		inS.Write(inElement.mInvBind);
	});
}

void SoftBodySharedSettings::RestoreBinaryState(StreamIn &inStream)
{
	inStream.Read(mVertices);
	inStream.Read(mFaces);
	inStream.Read(mEdgeConstraints);
	inStream.Read(mEdgeGroupEndIndices);
	inStream.Read(mVolumeConstraints);
	inStream.Read(mSkinnedConstraints);
	inStream.Read(mSkinnedConstraintNormals);
	inStream.Read(mLRAConstraints);
	inStream.Read(mVertexRadius);

	inStream.Read(mInvBindMatrices, [](StreamIn &inS, InvBind &outElement) {
		inS.Read(outElement.mJointIndex);
		inS.Read(outElement.mInvBind);
	});
}

void SoftBodySharedSettings::SaveWithMaterials(StreamOut &inStream, SharedSettingsToIDMap &ioSettingsMap, MaterialToIDMap &ioMaterialMap) const
{
	SharedSettingsToIDMap::const_iterator settings_iter = ioSettingsMap.find(this);
	if (settings_iter == ioSettingsMap.end())
	{
		// Write settings ID
		uint32 settings_id = (uint32)ioSettingsMap.size();
		ioSettingsMap[this] = settings_id;
		inStream.Write(settings_id);

		// Write the settings
		SaveBinaryState(inStream);

		// Write materials
		StreamUtils::SaveObjectArray(inStream, mMaterials, &ioMaterialMap);
	}
	else
	{
		// Known settings, just write the ID
		inStream.Write(settings_iter->second);
	}
}

SoftBodySharedSettings::SettingsResult SoftBodySharedSettings::sRestoreWithMaterials(StreamIn &inStream, IDToSharedSettingsMap &ioSettingsMap, IDToMaterialMap &ioMaterialMap)
{
	SettingsResult result;

	// Read settings id
	uint32 settings_id;
	inStream.Read(settings_id);
	if (inStream.IsEOF() || inStream.IsFailed())
	{
		result.SetError("Failed to read settings id");
		return result;
	}

	// Check nullptr settings
	if (settings_id == ~uint32(0))
	{
		result.Set(nullptr);
		return result;
	}

	// Check if we already read this settings
	if (settings_id < ioSettingsMap.size())
	{
		result.Set(ioSettingsMap[settings_id]);
		return result;
	}

	// Create new object
	Ref<SoftBodySharedSettings> settings = new SoftBodySharedSettings;

	// Read state
	settings->RestoreBinaryState(inStream);

	// Read materials
	Result mlresult = StreamUtils::RestoreObjectArray<PhysicsMaterialList>(inStream, ioMaterialMap);
	if (mlresult.HasError())
	{
		result.SetError(mlresult.GetError());
		return result;
	}
	settings->mMaterials = mlresult.Get();

	// Add the settings to the map
	ioSettingsMap.push_back(settings);

	result.Set(settings);
	return result;
}

JPH_NAMESPACE_END
