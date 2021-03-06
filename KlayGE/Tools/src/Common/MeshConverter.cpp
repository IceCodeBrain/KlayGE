/**
 * @file MeshConverter.cpp
 * @author Minmin Gong
 *
 * @section DESCRIPTION
 *
 * This source file is part of KlayGE
 * For the latest info, see http://www.klayge.org
 *
 * @section LICENSE
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published
 * by the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 *
 * You may alternatively use this source under the terms of
 * the KlayGE Proprietary License (KPL). You can obtained such a license
 * from http://www.klayge.org/licensing/.
 */

#include <KlayGE/KlayGE.hpp>
#include <KFL/CXX17/filesystem.hpp>
#include <KFL/ErrorHandling.hpp>
#include <KFL/Hash.hpp>
#include <KFL/Math.hpp>
#include <KFL/XMLDom.hpp>
#include <KlayGE/Mesh.hpp>
#include <KlayGE/RenderMaterial.hpp>
#include <KlayGE/ResLoader.hpp>

#include <cstring>
#include <iostream>

#if defined(KLAYGE_COMPILER_CLANGC2)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmicrosoft-enum-value" // Ignore int enum
#endif
#include <assimp/cimport.h>
#if defined(KLAYGE_COMPILER_CLANGC2)
#pragma clang diagnostic pop
#endif
#include <assimp/cexport.h>
#include <assimp/postprocess.h>
#include <assimp/Importer.hpp>
#include <assimp/scene.h>

#if defined(KLAYGE_COMPILER_CLANGC2)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-variable" // Ignore unused variable (mpl_assertion_in_line_xxx) in boost
#endif
#include <boost/algorithm/string/split.hpp>
#if defined(KLAYGE_COMPILER_CLANGC2)
#pragma clang diagnostic pop
#endif
#include <boost/algorithm/string/trim.hpp>

#include <KlayGE/MeshConverter.hpp>

using namespace std;
using namespace KlayGE;

namespace
{
	float3 Color4ToFloat3(aiColor4D const & c)
	{
		float3 v;
		v.x() = c.r;
		v.y() = c.g;
		v.z() = c.b;
		return v;
	}

	float3 AiVectorToFloat3(aiVector3D const& v)
	{
		return float3(v.x, v.y, v.z);
	}

	Quaternion AiQuatToQuat(aiQuaternion const& v)
	{
		return Quaternion(v.x, v.y, v.z, v.w);
	}

	template <typename T>
	float GetInterpTime(std::vector<T> const & vec, float time, size_t& itime_lower, size_t& itime_upper)
	{
		BOOST_ASSERT(!vec.empty());

		if (vec.size() == 1)
		{
			itime_lower = 0;
			itime_upper = 0;
			return 0;
		}

		// use @itime_upper as a hint to speed up find
		size_t vec_size = vec.size();
		size_t i = 0;
		for (i = itime_upper; i < vec_size; ++ i)
		{
			if (vec[i].first >= time)
			{
				break;
			}
		}

		if (i == 0)
		{
			itime_lower = 0;
			itime_upper = 1;
		}
		else if (i >= vec.size() - 1)
		{
			itime_lower = vec_size - 2;
			itime_upper = vec_size - 1;
		}
		else
		{
			itime_lower = i - 1;
			itime_upper = i;
		}

		float diff = vec[itime_upper].first - vec[itime_lower].first;
		return MathLib::clamp((diff == 0) ? 0 : (time - vec[itime_lower].first) / diff, 0.0f, 1.0f);
	}

	void MatrixToDQ(float4x4 const & mat, Quaternion& bind_real, Quaternion& bind_dual, float& bind_scale)
	{
		float4x4 tmp_mat = mat;
		float flip = 1;
		if (MathLib::dot(MathLib::cross(float3(tmp_mat(0, 0), tmp_mat(0, 1), tmp_mat(0, 2)),
			float3(tmp_mat(1, 0), tmp_mat(1, 1), tmp_mat(1, 2))),
			float3(tmp_mat(2, 0), tmp_mat(2, 1), tmp_mat(2, 2))) < 0)
		{
			tmp_mat(2, 0) = -tmp_mat(2, 0);
			tmp_mat(2, 1) = -tmp_mat(2, 1);
			tmp_mat(2, 2) = -tmp_mat(2, 2);

			flip = -1;
		}

		float3 scale;
		float3 trans;
		MathLib::decompose(scale, bind_real, trans, tmp_mat);

		bind_dual = MathLib::quat_trans_to_udq(bind_real, trans);

		if (flip * MathLib::SignBit(bind_real.w()) < 0)
		{
			bind_real = -bind_real;
			bind_dual = -bind_dual;
		}

		bind_scale = scale.x();
	}

	template <int N>
	void ExtractFVector(std::string_view value_str, float* v)
	{
		std::vector<std::string> strs;
		boost::algorithm::split(strs, value_str, boost::is_any_of(" "));
		for (size_t i = 0; i < N; ++ i)
		{
			if (i < strs.size())
			{
				boost::algorithm::trim(strs[i]);
				v[i] = static_cast<float>(atof(strs[i].c_str()));
			}
			else
			{
				v[i] = 0;
			}
		}
	}

	template <int N>
	void ExtractUIVector(std::string_view value_str, uint32_t* v)
	{
		std::vector<std::string> strs;
		boost::algorithm::split(strs, value_str, boost::is_any_of(" "));
		for (size_t i = 0; i < N; ++ i)
		{
			if (i < strs.size())
			{
				boost::algorithm::trim(strs[i]);
				v[i] = static_cast<uint32_t>(atoi(strs[i].c_str()));
			}
			else
			{
				v[i] = 0;
			}
		}
	}
}

namespace KlayGE
{
	void MeshConverter::RecursiveTransformMesh(uint32_t num_lods, uint32_t lod, float4x4 const & parent_mat, aiNode const * node)
	{
		auto const trans_mat = MathLib::transpose(float4x4(&node->mTransformation.a1)) * parent_mat;

		if (node->mNumMeshes > 0)
		{
			if (lod == 0)
			{
				NodeTransform node_transform;
				node_transform.name = node->mName.C_Str();
				node_transform.mesh_indices.assign(node->mMeshes, node->mMeshes + node->mNumMeshes);
				node_transform.lod_transforms.resize(num_lods);
				node_transform.lod_transforms[0] = trans_mat;

				nodes_.push_back(node_transform);
			}
			else
			{
				bool found = false;
				for (auto& node_transform : nodes_)
				{
					if (node_transform.name == node->mName.C_Str())
					{
						node_transform.lod_transforms[lod] = trans_mat;
						found = true;

						break;
					}
				}

				if (!found)
				{
					LogError() << "Could NOT find the correspondence node between LoDs" << std::endl;
					Verify(false);
				}
			}
		}

		for (uint32_t i = 0; i < node->mNumChildren; ++ i)
		{
			this->RecursiveTransformMesh(num_lods, lod, trans_mat, node->mChildren[i]);
		}
	}

	void MeshConverter::BuildMaterials(aiScene const * scene)
	{
		render_model_->NumMaterials(scene->mNumMaterials);

		for (unsigned int mi = 0; mi < scene->mNumMaterials; ++ mi)
		{
			std::string name;
			float3 albedo(0, 0, 0);
			float metalness = 0;
			float shininess = 1;
			float3 emissive(0, 0, 0);
			float opacity = 1;
			bool transparent = false;
			bool two_sided = false;

			aiString ai_name;
			aiColor4D ai_albedo;
			float ai_opacity;
			float ai_shininess;
			aiColor4D ai_emissive;
			int ai_two_sided;

			auto mtl = scene->mMaterials[mi];

			if (AI_SUCCESS == aiGetMaterialString(mtl, AI_MATKEY_NAME, &ai_name))
			{
				name = ai_name.C_Str();
			}

			if (AI_SUCCESS == aiGetMaterialColor(mtl, AI_MATKEY_COLOR_DIFFUSE, &ai_albedo))
			{
				albedo = Color4ToFloat3(ai_albedo);
			}
			{
				float3 specular(0, 0, 0);
				float strength = 1;
				aiColor4D ai_specular;

				// TODO: Restore metalness from specular color
				if (AI_SUCCESS == aiGetMaterialColor(mtl, AI_MATKEY_COLOR_SPECULAR, &ai_specular))
				{
					specular = Color4ToFloat3(ai_specular);
				}
				if (AI_SUCCESS == aiGetMaterialFloat(mtl, AI_MATKEY_SHININESS_STRENGTH, &strength))
				{
					specular *= strength;
				}
			}
			if (AI_SUCCESS == aiGetMaterialColor(mtl, AI_MATKEY_COLOR_EMISSIVE, &ai_emissive))
			{
				emissive = Color4ToFloat3(ai_emissive);
			}

			if (AI_SUCCESS == aiGetMaterialFloat(mtl, AI_MATKEY_OPACITY, &ai_opacity))
			{
				opacity = ai_opacity;
			}

			if (AI_SUCCESS == aiGetMaterialFloat(mtl, AI_MATKEY_SHININESS, &ai_shininess))
			{
				shininess = ai_shininess;
			}
			shininess = MathLib::clamp(shininess, 1.0f, MAX_SHININESS);

			if ((opacity < 1) || (aiGetMaterialTextureCount(mtl, aiTextureType_OPACITY) > 0))
			{
				transparent = true;
			}

			if (AI_SUCCESS == aiGetMaterialInteger(mtl, AI_MATKEY_TWOSIDED, &ai_two_sided))
			{
				two_sided = ai_two_sided ? true : false;
			}

			render_model_->GetMaterial(mi) = MakeSharedPtr<RenderMaterial>();
			auto& render_mtl = *render_model_->GetMaterial(mi);
			render_mtl.name = name;
			render_mtl.albedo = float4(albedo.x(), albedo.y(), albedo.z(), opacity);
			render_mtl.metalness = metalness;
			render_mtl.glossiness = Shininess2Glossiness(shininess);
			render_mtl.emissive = emissive;
			render_mtl.transparent = transparent;
			render_mtl.alpha_test = 0;
			render_mtl.sss = false;
			render_mtl.two_sided = two_sided;

			unsigned int count = aiGetMaterialTextureCount(mtl, aiTextureType_DIFFUSE);
			if (count > 0)
			{
				aiString str;
				aiGetMaterialTexture(mtl, aiTextureType_DIFFUSE, 0, &str, 0, 0, 0, 0, 0, 0);

				render_mtl.tex_names[RenderMaterial::TS_Albedo] = str.C_Str();
			}

			count = aiGetMaterialTextureCount(mtl, aiTextureType_SHININESS);
			if (count > 0)
			{
				aiString str;
				aiGetMaterialTexture(mtl, aiTextureType_SHININESS, 0, &str, 0, 0, 0, 0, 0, 0);

				render_mtl.tex_names[RenderMaterial::TS_Glossiness] = str.C_Str();
			}

			count = aiGetMaterialTextureCount(mtl, aiTextureType_EMISSIVE);
			if (count > 0)
			{
				aiString str;
				aiGetMaterialTexture(mtl, aiTextureType_EMISSIVE, 0, &str, 0, 0, 0, 0, 0, 0);

				render_mtl.tex_names[RenderMaterial::TS_Emissive] = str.C_Str();
			}

			count = aiGetMaterialTextureCount(mtl, aiTextureType_NORMALS);
			if (count > 0)
			{
				aiString str;
				aiGetMaterialTexture(mtl, aiTextureType_NORMALS, 0, &str, 0, 0, 0, 0, 0, 0);

				render_mtl.tex_names[RenderMaterial::TS_Normal] = str.C_Str();
			}

			count = aiGetMaterialTextureCount(mtl, aiTextureType_HEIGHT);
			if (count > 0)
			{
				aiString str;
				aiGetMaterialTexture(mtl, aiTextureType_HEIGHT, 0, &str, 0, 0, 0, 0, 0, 0);

				render_mtl.tex_names[RenderMaterial::TS_Height] = str.C_Str();
			}

			render_mtl.detail_mode = RenderMaterial::SDM_Parallax;
			if (render_mtl.tex_names[RenderMaterial::TS_Height].empty())
			{
				render_mtl.height_offset_scale = float2(0, 0);
			}
			else
			{
				render_mtl.height_offset_scale = float2(-0.5f, 0.06f);

				float ai_bumpscaling = 0;
				if (AI_SUCCESS == aiGetMaterialFloat(mtl, AI_MATKEY_BUMPSCALING, &ai_bumpscaling))
				{
					render_mtl.height_offset_scale.y() = ai_bumpscaling;
				}
			}
			render_mtl.tess_factors = float4(5, 5, 1, 9);
		}
	}

	void MeshConverter::BuildMeshData(std::vector<std::shared_ptr<aiScene const>> const & scene_lods)
	{
		for (size_t lod = 0; lod < scene_lods.size(); ++ lod)
		{
			for (unsigned int mi = 0; mi < scene_lods[lod]->mNumMeshes; ++ mi)
			{
				aiMesh const * mesh = scene_lods[lod]->mMeshes[mi];

				if (lod == 0)
				{
					meshes_[mi].mtl_id = mesh->mMaterialIndex;
					meshes_[mi].name = mesh->mName.C_Str();
				}

				auto& indices = meshes_[mi].lods[lod].indices;
				for (unsigned int fi = 0; fi < mesh->mNumFaces; ++ fi)
				{
					BOOST_ASSERT(3 == mesh->mFaces[fi].mNumIndices);

					indices.push_back(mesh->mFaces[fi].mIndices[0]);
					indices.push_back(mesh->mFaces[fi].mIndices[1]);
					indices.push_back(mesh->mFaces[fi].mIndices[2]);
				}

				bool has_normal = (mesh->mNormals != nullptr);
				bool has_tangent = (mesh->mTangents != nullptr);
				bool has_binormal = (mesh->mBitangents != nullptr);
				auto& has_texcoord = meshes_[mi].has_texcoord;
				uint32_t first_texcoord = AI_MAX_NUMBER_OF_TEXTURECOORDS;
				for (unsigned int tci = 0; tci < AI_MAX_NUMBER_OF_TEXTURECOORDS; ++ tci)
				{
					has_texcoord[tci] = (mesh->mTextureCoords[tci] != nullptr);
					if (has_texcoord[tci] && (AI_MAX_NUMBER_OF_TEXTURECOORDS == first_texcoord))
					{
						first_texcoord = tci;
					}
				}

				auto& positions = meshes_[mi].lods[lod].positions;
				auto& normals = meshes_[mi].lods[lod].normals;
				std::vector<float3> tangents(mesh->mNumVertices);
				std::vector<float3> binormals(mesh->mNumVertices);
				auto& texcoords = meshes_[mi].lods[lod].texcoords;
				positions.resize(mesh->mNumVertices);
				normals.resize(mesh->mNumVertices);
				for (unsigned int tci = 0; tci < AI_MAX_NUMBER_OF_TEXTURECOORDS; ++ tci)
				{
					texcoords[tci].resize(mesh->mNumVertices);
				}
				for (unsigned int vi = 0; vi < mesh->mNumVertices; ++ vi)
				{
					positions[vi] = float3(&mesh->mVertices[vi].x);

					if (has_normal)
					{
						normals[vi] = float3(&mesh->mNormals[vi].x);
					}
					if (has_tangent)
					{
						tangents[vi] = float3(&mesh->mTangents[vi].x);
					}
					if (has_binormal)
					{
						binormals[vi] = float3(&mesh->mBitangents[vi].x);
					}

					for (unsigned int tci = 0; tci < AI_MAX_NUMBER_OF_TEXTURECOORDS; ++ tci)
					{
						if (has_texcoord[tci])
						{
							BOOST_ASSERT(mesh->mTextureCoords[tci] != nullptr);
							KLAYGE_ASSUME(mesh->mTextureCoords[tci] != nullptr);

							texcoords[tci][vi] = float3(&mesh->mTextureCoords[tci][vi].x);
						}
					}
				}

				if (!has_normal)
				{
					MathLib::compute_normal(normals.begin(), indices.begin(), indices.end(), positions.begin(), positions.end());

					has_normal = true;
				}

				auto& mesh_tangents = meshes_[mi].lods[lod].tangents;
				auto& mesh_binormals = meshes_[mi].lods[lod].binormals;
				mesh_tangents.resize(mesh->mNumVertices);
				mesh_binormals.resize(mesh->mNumVertices);
				if ((!has_tangent || !has_binormal) && (first_texcoord != AI_MAX_NUMBER_OF_TEXTURECOORDS))
				{
					MathLib::compute_tangent(mesh_tangents.begin(), mesh_binormals.begin(), indices.begin(), indices.end(),
						positions.begin(), positions.end(), texcoords[first_texcoord].begin(), normals.begin());

					has_tangent = true;
				}

				meshes_[mi].has_normal = has_normal;
				meshes_[mi].has_tangent_frame = has_tangent || has_binormal;

				if (meshes_[mi].has_tangent_frame)
				{
					has_tangent_quat_ = true;
				}
				else if (meshes_[mi].has_normal)
				{
					has_normal = true;
				}
				if (first_texcoord != AI_MAX_NUMBER_OF_TEXTURECOORDS)
				{
					has_texcoord_ = true;
				}

				if (mesh->mNumBones > 0)
				{
					meshes_[mi].lods[lod].joint_bindings.resize(mesh->mNumVertices);

					for (unsigned int bi = 0; bi < mesh->mNumBones; ++ bi)
					{
						aiBone* bone = mesh->mBones[bi];
						bool found = false;
						for (uint32_t ji = 0; ji < joints_.size(); ++ ji)
						{
							if (joints_[ji].name == bone->mName.C_Str())
							{
								for (unsigned int wi = 0; wi < bone->mNumWeights; ++ wi)
								{
									float const weight = bone->mWeights[wi].mWeight;
									if (weight >= 0.5f / 255)
									{
										int const vertex_id = bone->mWeights[wi].mVertexId;
										meshes_[mi].lods[lod].joint_bindings[vertex_id].push_back({ ji, weight });
									}
								}

								found = true;
								break;
							}
						}
						if (!found)
						{
							BOOST_ASSERT_MSG(false, "Joint not found!");
						}
					}

					for (auto& binding : meshes_[mi].lods[lod].joint_bindings)
					{
						std::sort(binding.begin(), binding.end(),
							[](std::pair<uint32_t, float> const & lhs, std::pair<uint32_t, float> const & rhs)
							{
								return lhs.second > rhs.second;
							});
					}
				}
			}

			for (unsigned int mi = 0; mi < scene_lods[lod]->mNumMeshes; ++ mi)
			{
				if (has_tangent_quat_ && !meshes_[mi].has_tangent_frame)
				{
					meshes_[mi].has_tangent_frame = true;
				}
				if (has_texcoord_ && !meshes_[mi].has_texcoord[0])
				{
					meshes_[mi].has_texcoord[0] = true;
				}
			}
		}
	}

	void MeshConverter::BuildJoints(aiScene const * scene)
	{
		std::map<std::string, Joint> joint_nodes;

		std::function<void(aiNode const *, float4x4 const &)> build_bind_matrix =
			[&build_bind_matrix, &joint_nodes, scene](aiNode const * node, float4x4 const & parent_mat)
		{
			float4x4 const mesh_trans = MathLib::transpose(float4x4(&node->mTransformation.a1)) * parent_mat;
			for (unsigned int i = 0; i < node->mNumMeshes; ++ i)
			{
				aiMesh const * mesh = scene->mMeshes[node->mMeshes[i]];
				for (unsigned int ibone = 0; ibone < mesh->mNumBones; ++ ibone)
				{
					aiBone const * bone = mesh->mBones[ibone];

					Joint joint;
					joint.name = bone->mName.C_Str();

					auto const bone_to_mesh = MathLib::inverse(MathLib::transpose(float4x4(&bone->mOffsetMatrix.a1))) * mesh_trans;					
					MatrixToDQ(bone_to_mesh, joint.bind_real, joint.bind_dual, joint.bind_scale);

					joint_nodes[joint.name] = joint;
				}
			}

			for (unsigned int i = 0; i < node->mNumChildren; ++ i)
			{
				build_bind_matrix(node->mChildren[i], mesh_trans);
			}
		};

		std::function<bool(aiNode const *)> mark_joint_nodes = [&mark_joint_nodes, &joint_nodes](aiNode const * node)
		{
			std::string name = node->mName.C_Str();
			bool child_has_bone = false;

			auto iter = joint_nodes.find(name);
			if (iter != joint_nodes.end())
			{
				child_has_bone = true;
			}

			for (unsigned int i = 0; i < node->mNumChildren; ++ i)
			{
				child_has_bone = mark_joint_nodes(node->mChildren[i]) || child_has_bone;
			}

			if (child_has_bone && (iter == joint_nodes.end()))
			{
				Joint joint;
				joint.name = name;
				joint.bind_real = Quaternion::Identity();
				joint.bind_dual = Quaternion(0, 0, 0, 0);
				joint.bind_scale = 1;
				joint_nodes[name] = joint;
			}

			return child_has_bone;
		};

		std::function<void(aiNode const *, int)> alloc_joints =
			[this, &joint_nodes, &alloc_joints](aiNode const * node, int parent_id)
		{
			std::string name = node->mName.C_Str();
			int joint_id = -1;
			auto iter = joint_nodes.find(name);
			if (iter != joint_nodes.end())
			{
				joint_id = static_cast<int>(joints_.size());

				auto const local_matrix = MathLib::transpose(float4x4(&node->mTransformation.a1));
				// Borrow those variables to store a local matrix
				MatrixToDQ(local_matrix, iter->second.inverse_origin_real, iter->second.inverse_origin_dual,
					iter->second.inverse_origin_scale);

				iter->second.parent = static_cast<int16_t>(parent_id);

				joints_.push_back(iter->second);
			}

			for (unsigned int i = 0; i < node->mNumChildren; ++ i)
			{
				alloc_joints(node->mChildren[i], joint_id);
			}
		};

		build_bind_matrix(scene->mRootNode, float4x4::Identity());
		mark_joint_nodes(scene->mRootNode);
		alloc_joints(scene->mRootNode, -1);
	}

	void MeshConverter::BuildActions(aiScene const * scene)
	{
		auto& skinned_model = *checked_pointer_cast<SkinnedModel>(render_model_);

		struct Animation
		{
			std::string name;
			int frame_num;
			std::map<int/*joint_id*/, KeyFrameSet> resampled_frames;
		};

		std::vector<Animation> animations;

		int const resample_fps = 25;
		// for actions
		for (unsigned int ianim = 0; ianim < scene->mNumAnimations; ++ ianim)
		{
			aiAnimation const * cur_anim = scene->mAnimations[ianim];
			float duration = static_cast<float>(cur_anim->mDuration / cur_anim->mTicksPerSecond);
			Animation anim;
			anim.name = cur_anim->mName.C_Str();
			anim.frame_num = static_cast<int>(ceilf(duration * resample_fps));
			if (anim.frame_num == 0)
			{
				anim.frame_num = 1;
			}

			// import joints animation
			for (unsigned int ichannel = 0; ichannel < cur_anim->mNumChannels; ++ ichannel)
			{
				aiNodeAnim const * cur_joint = cur_anim->mChannels[ichannel];

				int joint_id = -1;
				for (size_t ji = 0; ji < joints_.size(); ++ ji)
				{
					if (joints_[ji].name == cur_joint->mNodeName.C_Str())
					{
						joint_id = static_cast<int>(ji);
						break;
					}
				}

				// NOTE: ignore animation if node is not joint
				if (joint_id > 0)
				{
					std::vector<std::pair<float, float3>> poss;
					for (unsigned int i = 0; i < cur_joint->mNumPositionKeys; ++ i)
					{
						auto const & p = cur_joint->mPositionKeys[i];
						poss.push_back(std::make_pair(static_cast<float>(p.mTime), AiVectorToFloat3(p.mValue)));
					}

					std::vector<std::pair<float, Quaternion>> quats;
					for (unsigned int i = 0; i < cur_joint->mNumRotationKeys; ++ i)
					{
						auto const & p = cur_joint->mRotationKeys[i];
						quats.push_back(std::make_pair(static_cast<float>(p.mTime), AiQuatToQuat(p.mValue)));
					}

					std::vector<std::pair<float, float3>> scales;
					for (unsigned int i = 0; i < cur_joint->mNumScalingKeys; ++ i)
					{
						auto const & p = cur_joint->mScalingKeys[i];
						scales.push_back(std::make_pair(static_cast<float>(p.mTime), AiVectorToFloat3(p.mValue)));
					}

					// resample
					this->ResampleJointTransform(anim.resampled_frames[joint_id], 0, anim.frame_num,
						static_cast<float>(cur_anim->mTicksPerSecond / resample_fps), poss, quats, scales);
				}
			}

			for (size_t ji = 0; ji < joints_.size(); ++ ji)
			{
				int joint_id = static_cast<int>(ji);
				if (anim.resampled_frames.find(joint_id) == anim.resampled_frames.end())
				{
					KeyFrameSet default_tf;
					default_tf.frame_id.push_back(0);
					// Borrow those variables to store a local matrix
					default_tf.bind_real.push_back(joints_[ji].inverse_origin_real);
					default_tf.bind_dual.push_back(joints_[ji].inverse_origin_dual);
					default_tf.bind_scale.push_back(joints_[ji].inverse_origin_scale);
					anim.resampled_frames.emplace(joint_id, default_tf);
				}
			}

			animations.push_back(anim);
		}

		auto kfs = MakeSharedPtr<std::vector<KeyFrameSet>>(joints_.size());
		auto actions = MakeSharedPtr<std::vector<AnimationAction>>();
		int action_frame_offset = 0;
		for (auto const & anim : animations)
		{
			AnimationAction action;
			action.name = anim.name;
			action.start_frame = action_frame_offset;
			action.end_frame = action_frame_offset + anim.frame_num;
			actions->push_back(action);

			for (auto const & frame : anim.resampled_frames)
			{
				auto& kf = (*kfs)[frame.first];
				for (size_t f = 0; f < frame.second.frame_id.size(); ++ f)
				{
					int const shifted_frame = frame.second.frame_id[f] + action_frame_offset;

					kf.frame_id.push_back(shifted_frame);
					kf.bind_real.push_back(frame.second.bind_real[f]);
					kf.bind_dual.push_back(frame.second.bind_dual[f]);
					kf.bind_scale.push_back(frame.second.bind_scale[f]);
				}

				this->CompressKeyFrameSet(kf);
			}

			action_frame_offset = action_frame_offset + anim.frame_num;
		}

		skinned_model.AttachKeyFrameSets(kfs);
		skinned_model.AttachActions(actions);

		skinned_model.FrameRate(resample_fps);
		skinned_model.NumFrames(action_frame_offset);
	}

	void MeshConverter::ResampleJointTransform(KeyFrameSet& rkf, int start_frame, int end_frame, float fps_scale,
		std::vector<std::pair<float, float3>> const & poss, std::vector<std::pair<float, Quaternion>> const & quats,
		std::vector<std::pair<float, float3>> const & scales)
	{
		size_t i_pos = 0;
		size_t i_rot = 0;
		size_t i_scale = 0;
		for (int i = start_frame; i < end_frame; ++ i)
		{
			float time = i * fps_scale;
			size_t prev_i = 0;
			float fraction = 0.0f;
			float3 scale_resampled(1, 1, 1);
			Quaternion bind_real_resampled(0, 0, 0, 1);
			Quaternion bind_dual_resampled(0, 0, 0, 0);

			if (!scales.empty())
			{
				fraction = GetInterpTime(scales, time, prev_i, i_scale);
				scale_resampled = MathLib::lerp(scales[prev_i].second, scales[i_scale].second, fraction);
			}
			if (!quats.empty())
			{
				fraction = GetInterpTime(quats, time, prev_i, i_rot);
				bind_real_resampled = MathLib::slerp(quats[prev_i].second, quats[i_rot].second, fraction);
			}
			if (!poss.empty())
			{
				fraction = GetInterpTime(poss, time, prev_i, i_pos);

				auto bind_dual_prev_i = MathLib::quat_trans_to_udq(quats[prev_i].second, poss[prev_i].second);
				auto bind_dual_i_pos = MathLib::quat_trans_to_udq(quats[i_rot].second, poss[i_pos].second);

				auto bind_dq_resampled = MathLib::sclerp(quats[prev_i].second, bind_dual_prev_i,
					quats[i_rot].second, bind_dual_i_pos,
					fraction);

				bind_dual_resampled = bind_dq_resampled.second;
			}

			if (MathLib::SignBit(bind_real_resampled.w()) < 0)
			{
				bind_real_resampled = -bind_real_resampled;
				bind_dual_resampled = -bind_dual_resampled;
			}

			rkf.frame_id.push_back(i);
			rkf.bind_real.push_back(bind_real_resampled);
			rkf.bind_dual.push_back(bind_dual_resampled);
			rkf.bind_scale.push_back(scale_resampled.x());
		}
	}

	void MeshConverter::LoadFromAssimp(std::string const & input_name, MeshMetadata const & metadata)
	{
		auto ai_property_store_deleter = [](aiPropertyStore* props)
		{
			aiReleasePropertyStore(props);
		};

		std::unique_ptr<aiPropertyStore, decltype(ai_property_store_deleter)> props(aiCreatePropertyStore(), ai_property_store_deleter);
		aiSetImportPropertyInteger(props.get(), AI_CONFIG_IMPORT_TER_MAKE_UVS, 1);
		aiSetImportPropertyFloat(props.get(), AI_CONFIG_PP_GSN_MAX_SMOOTHING_ANGLE, 80);
		aiSetImportPropertyInteger(props.get(), AI_CONFIG_PP_SBP_REMOVE, 0);

		aiSetImportPropertyInteger(props.get(), AI_CONFIG_GLOB_MEASURE_TIME, 1);

		unsigned int ppsteps = aiProcess_JoinIdenticalVertices // join identical vertices/ optimize indexing
			| aiProcess_ValidateDataStructure // perform a full validation of the loader's output
			| aiProcess_RemoveRedundantMaterials // remove redundant materials
			| aiProcess_FindInstances; // search for instanced meshes and remove them by references to one master

		uint32_t const num_lods = static_cast<uint32_t>(metadata.NumLods());

		auto ai_scene_deleter = [](aiScene const * scene)
		{
			aiReleaseImport(scene);
		};

		std::vector<std::shared_ptr<aiScene const>> scenes(num_lods);
		for (uint32_t lod = 0; lod < num_lods; ++ lod)
		{
			std::string_view const lod_file_name = (lod == 0) ? input_name : metadata.LodFileName(lod);
			std::string const file_name = (lod == 0) ? input_name : ResLoader::Instance().Locate(lod_file_name);
			if (file_name.empty())
			{
				LogError() << "Could NOT find " << lod_file_name << " for LoD " << lod << '.' << std::endl;
				return;
			}

			scenes[lod].reset(aiImportFileExWithProperties(file_name.c_str(),
				ppsteps // configurable pp steps
				| aiProcess_GenSmoothNormals // generate smooth normal vectors if not existing
				| aiProcess_Triangulate // triangulate polygons with more than 3 edges
				| aiProcess_ConvertToLeftHanded // convert everything to D3D left handed space
				| aiProcess_FixInfacingNormals, // find normals facing inwards and inverts them
				nullptr, props.get()), ai_scene_deleter);

			if (!scenes[lod])
			{
				LogError() << "Assimp: Import file " << lod_file_name << " error: " << aiGetErrorString() << std::endl;
				return;
			}
		}

		this->BuildJoints(scenes[0].get());

		bool const skinned = !joints_.empty();

		if (skinned)
		{
			render_model_ = MakeSharedPtr<SkinnedModel>(L"Software");
		}
		else
		{
			render_model_ = MakeSharedPtr<RenderModel>(L"Software");
		}

		this->BuildMaterials(scenes[0].get());

		meshes_.resize(scenes[0]->mNumMeshes);
		for (size_t mi = 0; mi < meshes_.size(); ++ mi)
		{
			meshes_[mi].lods.resize(num_lods);
		}

		this->BuildMeshData(scenes);
		for (uint32_t lod = 0; lod < num_lods; ++ lod)
		{
			this->RecursiveTransformMesh(num_lods, lod, float4x4::Identity(), scenes[lod]->mRootNode);
		}

		if (skinned)
		{
			this->BuildActions(scenes[0].get());
		}

		for (auto& mesh : meshes_)
		{
			auto& lod0 = mesh.lods[0];

			mesh.pos_bb = MathLib::compute_aabbox(lod0.positions.begin(), lod0.positions.end());
			mesh.tc_bb = MathLib::compute_aabbox(lod0.texcoords[0].begin(), lod0.texcoords[0].end());
		}
	}

	void MeshConverter::SaveByAssimp(std::string const & output_name)
	{
		std::vector<aiScene> scene_lods(render_model_->NumLods());
		for (uint32_t lod = 0; lod < render_model_->NumLods(); ++ lod)
		{
			auto& ai_scene = scene_lods[lod];

			ai_scene.mNumMaterials = static_cast<uint32_t>(render_model_->NumMaterials());
			ai_scene.mMaterials = new aiMaterial*[ai_scene.mNumMaterials];
			for (uint32_t i = 0; i < ai_scene.mNumMaterials; ++ i)
			{
				auto const & mtl = *render_model_->GetMaterial(i);

				ai_scene.mMaterials[i] = new aiMaterial;
				auto& ai_mtl = *ai_scene.mMaterials[i];

				{
					aiString name;
					name.Set(mtl.name.c_str());
					ai_mtl.AddProperty(&name, AI_MATKEY_NAME);
				}

				{
					float3 const diffuse = mtl.albedo * (1 - mtl.metalness);
					float3 const specular = MathLib::lerp(float3(0.04f, 0.04f, 0.04f),
						float3(mtl.albedo.x(), mtl.albedo.y(), mtl.albedo.z()), mtl.metalness);

					aiColor3D const ai_diffuse(diffuse.x(), diffuse.y(), diffuse.z());
					ai_mtl.AddProperty(&ai_diffuse, 1, AI_MATKEY_COLOR_DIFFUSE);

					float const ai_shininess_strength = MathLib::max3(specular.x(), specular.y(), specular.z());
					ai_mtl.AddProperty(&ai_shininess_strength, 1, AI_MATKEY_SHININESS_STRENGTH);

					aiColor3D const ai_specular(specular.x() / ai_shininess_strength, specular.y() / ai_shininess_strength,
						specular.z() / ai_shininess_strength);
					ai_mtl.AddProperty(&ai_specular, 1, AI_MATKEY_COLOR_SPECULAR);
				}
				{
					aiColor3D const ai_emissive(mtl.emissive.x(), mtl.emissive.y(), mtl.emissive.z());
					ai_mtl.AddProperty(&ai_emissive, 1, AI_MATKEY_COLOR_EMISSIVE);
				}

				{
					ai_real const ai_opacity = mtl.albedo.w();
					ai_mtl.AddProperty(&ai_opacity, 1, AI_MATKEY_OPACITY);
				}

				{
					ai_real const ai_shininess = Glossiness2Shininess(mtl.glossiness);
					ai_mtl.AddProperty(&ai_shininess, 1, AI_MATKEY_SHININESS);
				}

				if (mtl.two_sided)
				{
					int const ai_two_sided = 1;
					ai_mtl.AddProperty(&ai_two_sided, 1, AI_MATKEY_TWOSIDED);
				}

				// TODO: alpha test, SSS

				if (!mtl.tex_names[RenderMaterial::TS_Albedo].empty())
				{
					aiString name;
					name.Set(mtl.tex_names[RenderMaterial::TS_Albedo]);
					ai_mtl.AddProperty(&name, AI_MATKEY_TEXTURE_DIFFUSE(0));
				}

				if (!mtl.tex_names[RenderMaterial::TS_Glossiness].empty())
				{
					aiString name;
					name.Set(mtl.tex_names[RenderMaterial::TS_Glossiness]);
					ai_mtl.AddProperty(&name, AI_MATKEY_TEXTURE_SHININESS(0));
				}

				if (!mtl.tex_names[RenderMaterial::TS_Emissive].empty())
				{
					aiString name;
					name.Set(mtl.tex_names[RenderMaterial::TS_Emissive]);
					ai_mtl.AddProperty(&name, AI_MATKEY_TEXTURE_EMISSIVE(0));
				}

				if (!mtl.tex_names[RenderMaterial::TS_Normal].empty())
				{
					aiString name;
					name.Set(mtl.tex_names[RenderMaterial::TS_Normal]);
					ai_mtl.AddProperty(&name, AI_MATKEY_TEXTURE_NORMALS(0));
				}

				if (!mtl.tex_names[RenderMaterial::TS_Height].empty())
				{
					aiString name;
					name.Set(mtl.tex_names[RenderMaterial::TS_Height]);
					ai_mtl.AddProperty(&name, AI_MATKEY_TEXTURE_HEIGHT(0));

					// TODO: AI_MATKEY_BUMPSCALING
				}
			}

			ai_scene.mNumMeshes = render_model_->NumSubrenderables();
			ai_scene.mMeshes = new aiMesh*[ai_scene.mNumMeshes];

			ai_scene.mRootNode = new aiNode;
			ai_scene.mRootNode->mNumChildren = ai_scene.mNumMeshes;
			ai_scene.mRootNode->mChildren = new aiNode*[ai_scene.mRootNode->mNumChildren];
			ai_scene.mRootNode->mNumMeshes = 0;
			ai_scene.mRootNode->mMeshes = nullptr;
			ai_scene.mRootNode->mParent = nullptr;

			for (uint32_t i = 0; i < ai_scene.mNumMeshes; ++ i)
			{
				auto const & mesh = *checked_cast<StaticMesh*>(render_model_->Subrenderable(i).get());

				ai_scene.mMeshes[i] = new aiMesh;
				auto& ai_mesh = *ai_scene.mMeshes[i];

				ai_mesh.mMaterialIndex = mesh.MaterialID();
				ai_mesh.mPrimitiveTypes = aiPrimitiveType_TRIANGLE;

				ai_scene.mRootNode->mChildren[i] = new aiNode;
				ai_scene.mRootNode->mChildren[i]->mNumMeshes = 1;
				ai_scene.mRootNode->mChildren[i]->mMeshes = new unsigned int[1];
				ai_scene.mRootNode->mChildren[i]->mMeshes[0] = i;
				ai_scene.mRootNode->mChildren[i]->mParent = ai_scene.mRootNode;
				ai_scene.mRootNode->mChildren[i]->mNumChildren = 0;
				ai_scene.mRootNode->mChildren[i]->mChildren = nullptr;

				std::string name;
				KlayGE::Convert(name, mesh.Name());
				ai_scene.mRootNode->mChildren[i]->mName.Set(name.c_str());

				auto const & rl = mesh.GetRenderLayout();

				ai_mesh.mNumVertices = mesh.NumVertices(lod);
				uint32_t const start_vertex = mesh.StartVertexLocation(lod);

				for (uint32_t vi = 0; vi < rl.NumVertexStreams(); ++ vi)
				{
					GraphicsBuffer::Mapper mapper(*rl.GetVertexStream(vi), BA_Read_Only);

					auto const & ve = rl.VertexStreamFormat(vi)[0];
					switch (ve.usage)
					{
					case VEU_Position:
						ai_mesh.mVertices = new aiVector3D[ai_mesh.mNumVertices];

						switch (ve.format)
						{
						case EF_SIGNED_ABGR16:
							{
								auto const & pos_bb = mesh.PosBound();
								float3 const pos_center = pos_bb.Center();
								float3 const pos_extent = pos_bb.HalfSize();

								int16_t const * p_16 = mapper.Pointer<int16_t>() + start_vertex * 4;
								for (uint32_t j = 0; j < ai_mesh.mNumVertices; ++ j)
								{
									ai_mesh.mVertices[j].x
										= ((p_16[j * 4 + 0] + 32768) / 65535.0f * 2 - 1) * pos_extent.x() + pos_center.x();
									ai_mesh.mVertices[j].y
										= ((p_16[j * 4 + 1] + 32768) / 65535.0f * 2 - 1) * pos_extent.y() + pos_center.y();
									ai_mesh.mVertices[j].z
										= ((p_16[j * 4 + 2] + 32768) / 65535.0f * 2 - 1) * pos_extent.z() + pos_center.z();
								}

								break;
							}

						case EF_BGR32F:
						case EF_ABGR32F:
							{
								uint32_t const num_elems = NumComponents(ve.format);
								float const * p_32f = mapper.Pointer<float>() + start_vertex * num_elems;
								for (uint32_t j = 0; j < ai_mesh.mNumVertices; ++ j)
								{
									ai_mesh.mVertices[j]
										= aiVector3D(p_32f[j * num_elems + 0], p_32f[j * num_elems + 1], p_32f[j * num_elems + 2]);
								}
								break;
							}

						default:
							KFL_UNREACHABLE("Unsupported position format.");
						}
						break;

					case VEU_Tangent:
						ai_mesh.mTangents = new aiVector3D[ai_mesh.mNumVertices];
						ai_mesh.mBitangents = new aiVector3D[ai_mesh.mNumVertices];
						ai_mesh.mNormals = new aiVector3D[ai_mesh.mNumVertices];

						switch (ve.format)
						{
						case EF_ABGR8:
							{
								uint8_t const * tangent_quats = mapper.Pointer<uint8_t>() + start_vertex * 4;
								for (uint32_t j = 0; j < ai_mesh.mNumVertices; ++ j)
								{
									Quaternion tangent_quat;
									tangent_quat.x() = (tangent_quats[j * 4 + 0] / 255.0f) * 2 - 1;
									tangent_quat.y() = (tangent_quats[j * 4 + 1] / 255.0f) * 2 - 1;
									tangent_quat.z() = (tangent_quats[j * 4 + 2] / 255.0f) * 2 - 1;
									tangent_quat.w() = (tangent_quats[j * 4 + 3] / 255.0f) * 2 - 1;
									tangent_quat = MathLib::normalize(tangent_quat);

									auto const tangent = MathLib::transform_quat(float3(1, 0, 0), tangent_quat);
									auto const binormal = MathLib::transform_quat(float3(0, 1, 0), tangent_quat)
										* MathLib::sgn(tangent_quat.w());
									auto const normal = MathLib::transform_quat(float3(0, 0, 1), tangent_quat);

									ai_mesh.mTangents[j] = aiVector3D(tangent.x(), tangent.y(), tangent.z());
									ai_mesh.mBitangents[j] = aiVector3D(binormal.x(), binormal.y(), binormal.z());
									ai_mesh.mNormals[j] = aiVector3D(normal.x(), normal.y(), normal.z());
								}
								break;
							}

						default:
							KFL_UNREACHABLE("Unsupported tangent frame format.");
						}
						break;

					case VEU_Normal:
						ai_mesh.mNormals = new aiVector3D[ai_mesh.mNumVertices];

						switch (ve.format)
						{
						case EF_ABGR8:
							{
								uint8_t const * normals = mapper.Pointer<uint8_t>() + start_vertex * 4;
								for (uint32_t j = 0; j < ai_mesh.mNumVertices; ++ j)
								{
									float3 normal;
									normal.x() = (normals[j * 4 + 0] / 255.0f) * 2 - 1;
									normal.y() = (normals[j * 4 + 1] / 255.0f) * 2 - 1;
									normal.z() = (normals[j * 4 + 2] / 255.0f) * 2 - 1;
									normal = MathLib::normalize(normal);

									ai_mesh.mNormals[j] = aiVector3D(normal.x(), normal.y(), normal.z());
								}
								break;
							}

						default:
							KFL_UNREACHABLE("Unsupported normal format.");
						}
						break;

					case VEU_Diffuse:
						ai_mesh.mColors[0] = new aiColor4D[ai_mesh.mNumVertices];

						switch (ve.format)
						{
						case EF_ABGR8:
							{
								uint8_t const * diffuses = mapper.Pointer<uint8_t>() + start_vertex * 4;
								for (uint32_t j = 0; j < ai_mesh.mNumVertices; ++ j)
								{
									aiColor4D diffuse;
									diffuse.r = (diffuses[j * 4 + 0] / 255.0f) * 2 - 1;
									diffuse.g = (diffuses[j * 4 + 1] / 255.0f) * 2 - 1;
									diffuse.b = (diffuses[j * 4 + 2] / 255.0f) * 2 - 1;
									diffuse.a = (diffuses[j * 4 + 3] / 255.0f) * 2 - 1;

									ai_mesh.mColors[0][j] = diffuse;
								}
								break;
							}

						case EF_ARGB8:
							{
								uint8_t const * diffuses = mapper.Pointer<uint8_t>() + start_vertex * 4;
								for (uint32_t j = 0; j < ai_mesh.mNumVertices; ++ j)
								{
									aiColor4D diffuse;
									diffuse.r = (diffuses[j * 4 + 2] / 255.0f) * 2 - 1;
									diffuse.g = (diffuses[j * 4 + 1] / 255.0f) * 2 - 1;
									diffuse.b = (diffuses[j * 4 + 0] / 255.0f) * 2 - 1;
									diffuse.a = (diffuses[j * 4 + 3] / 255.0f) * 2 - 1;

									ai_mesh.mColors[0][j] = diffuse;
								}
								break;
							}

						default:
							KFL_UNREACHABLE("Unsupported normal format.");
						}
						break;

					case VEU_Specular:
						ai_mesh.mColors[1] = new aiColor4D[ai_mesh.mNumVertices];

						switch (ve.format)
						{
						case EF_ABGR8:
							{
								uint8_t const * speculars = mapper.Pointer<uint8_t>() + start_vertex * 4;
								for (uint32_t j = 0; j < ai_mesh.mNumVertices; ++ j)
								{
									aiColor4D specular;
									specular.r = (speculars[j * 4 + 0] / 255.0f) * 2 - 1;
									specular.g = (speculars[j * 4 + 1] / 255.0f) * 2 - 1;
									specular.b = (speculars[j * 4 + 2] / 255.0f) * 2 - 1;
									specular.a = (speculars[j * 4 + 3] / 255.0f) * 2 - 1;

									ai_mesh.mColors[1][j] = specular;
								}
								break;
							}

						case EF_ARGB8:
							{
								uint8_t const * speculars = mapper.Pointer<uint8_t>() + start_vertex * 4;
								for (uint32_t j = 0; j < ai_mesh.mNumVertices; ++ j)
								{
									aiColor4D specular;
									specular.r = (speculars[j * 4 + 2] / 255.0f) * 2 - 1;
									specular.g = (speculars[j * 4 + 1] / 255.0f) * 2 - 1;
									specular.b = (speculars[j * 4 + 0] / 255.0f) * 2 - 1;
									specular.a = (speculars[j * 4 + 3] / 255.0f) * 2 - 1;

									ai_mesh.mColors[1][j] = specular;
								}
								break;
							}

						default:
							KFL_UNREACHABLE("Unsupported normal format.");
						}
						break;

					case VEU_TextureCoord:
						ai_mesh.mTextureCoords[ve.usage_index] = new aiVector3D[ai_mesh.mNumVertices];
						ai_mesh.mNumUVComponents[ve.usage_index] = 2;

						switch (ve.format)
						{
						case EF_SIGNED_GR16:
							{
								auto const & tc_bb = mesh.TexcoordBound();
								float3 const tc_center = tc_bb.Center();
								float3 const tc_extent = tc_bb.HalfSize();

								int16_t const * tc_16 = mapper.Pointer<int16_t>() + start_vertex * 2;
								for (uint32_t j = 0; j < ai_mesh.mNumVertices; ++ j)
								{
									ai_mesh.mTextureCoords[ve.usage_index][j].x
										= ((tc_16[j * 2 + 0] + 32768) / 65535.0f * 2 - 1) * tc_extent.x() + tc_center.x();
									ai_mesh.mTextureCoords[ve.usage_index][j].y
										= ((tc_16[j * 2 + 1] + 32768) / 65535.0f * 2 - 1) * tc_extent.y() + tc_center.y();
								}

								break;
							}

						case EF_GR32F:
							{
								float const * tc_32f = mapper.Pointer<float>() + start_vertex * 2;
								for (uint32_t j = 0; j < ai_mesh.mNumVertices; ++ j)
								{
									ai_mesh.mTextureCoords[ve.usage_index][j] = aiVector3D(tc_32f[j * 2 + 0], tc_32f[j * 2 + 1], 0);
								}
								break;
							}

						default:
							KFL_UNREACHABLE("Unsupported texcoord format.");
						}
						break;

					default:
						KFL_UNREACHABLE("Unsupported vertex format.");
					}
				}

				{
					ai_mesh.mNumFaces = mesh.NumIndices(lod) / 3;
					uint32_t const start_index = mesh.StartIndexLocation(lod);

					ai_mesh.mFaces = new aiFace[ai_mesh.mNumFaces];

					GraphicsBuffer::Mapper mapper(*rl.GetIndexStream(), BA_Read_Only);
					if (rl.IndexStreamFormat() == EF_R16UI)
					{
						auto const * indices_16 = mapper.Pointer<int16_t>() + start_index;

						for (uint32_t j = 0; j < ai_mesh.mNumFaces; ++ j)
						{
							auto& ai_face = ai_mesh.mFaces[j];

							ai_face.mIndices = new unsigned int[3];
							ai_face.mNumIndices = 3;

							ai_face.mIndices[0] = indices_16[j * 3 + 0];
							ai_face.mIndices[1] = indices_16[j * 3 + 1];
							ai_face.mIndices[2] = indices_16[j * 3 + 2];
						}
					}
					else
					{
						auto const * indices_32 = mapper.Pointer<int32_t>() + start_index;

						for (uint32_t j = 0; j < ai_mesh.mNumFaces; ++ j)
						{
							auto& ai_face = ai_mesh.mFaces[j];

							ai_face.mIndices = new unsigned int[3];
							ai_face.mNumIndices = 3;

							ai_face.mIndices[0] = indices_32[j * 3 + 0];
							ai_face.mIndices[1] = indices_32[j * 3 + 1];
							ai_face.mIndices[2] = indices_32[j * 3 + 2];
						}
					}
				}
			}

			auto const output_path = std::filesystem::path(output_name);
			auto const output_ext = output_path.extension();
			auto lod_output_name = (output_path.parent_path() / output_path.stem()).string();
			if (scene_lods.size() > 1)
			{
				lod_output_name  += "_lod_" + std::to_string(lod);
			}
			lod_output_name += output_ext.string();
			aiExportScene(&ai_scene, output_ext.string().substr(1).c_str(), lod_output_name.c_str(), 0);
		}
	}

	void MeshConverter::CompileMaterialsChunk(XMLNodePtr const & materials_chunk)
	{
		uint32_t num_mtls = 0;
		for (XMLNodePtr mtl_node = materials_chunk->FirstNode("material"); mtl_node;
			mtl_node = mtl_node->NextSibling("material"))
		{
			++ num_mtls;
		}

		render_model_->NumMaterials(num_mtls);

		uint32_t mtl_index = 0;
		for (XMLNodePtr mtl_node = materials_chunk->FirstNode("material"); mtl_node;
			mtl_node = mtl_node->NextSibling("material"), ++ mtl_index)
		{
			render_model_->GetMaterial(mtl_index) = MakeSharedPtr<RenderMaterial>();
			auto& mtl = *render_model_->GetMaterial(mtl_index);

			mtl.name = "Material " + std::to_string(mtl_index);

			mtl.albedo = float4(0, 0, 0, 1);
			mtl.metalness = 0;
			mtl.glossiness = 0;
			mtl.emissive = float3(0, 0, 0);
			mtl.transparent = false;
			mtl.alpha_test = 0;
			mtl.sss = false;
			mtl.two_sided = false;

			mtl.detail_mode = RenderMaterial::SDM_Parallax;
			mtl.height_offset_scale = float2(-0.5f, 0.06f);
			mtl.tess_factors = float4(5, 5, 1, 9);

			{
				XMLAttributePtr attr = mtl_node->Attrib("name");
				if (attr)
				{
					mtl.name = std::string(attr->ValueString());
				}
			}

			XMLNodePtr albedo_node = mtl_node->FirstNode("albedo");
			if (albedo_node)
			{
				XMLAttributePtr attr = albedo_node->Attrib("color");
				if (attr)
				{
					ExtractFVector<4>(attr->ValueString(), &mtl.albedo[0]);
				}
				attr = albedo_node->Attrib("texture");
				if (attr)
				{
					mtl.tex_names[RenderMaterial::TS_Albedo] = std::string(attr->ValueString());
				}
			}
			else
			{
				XMLAttributePtr attr = mtl_node->Attrib("diffuse");
				if (attr)
				{
					ExtractFVector<3>(attr->ValueString(), &mtl.albedo[0]);
				}
				else
				{
					attr = mtl_node->Attrib("diffuse_r");
					if (attr)
					{
						mtl.albedo.x() = attr->ValueFloat();
					}
					attr = mtl_node->Attrib("diffuse_g");
					if (attr)
					{
						mtl.albedo.y() = attr->ValueFloat();
					}
					attr = mtl_node->Attrib("diffuse_b");
					if (attr)
					{
						mtl.albedo.z() = attr->ValueFloat();
					}
				}

				attr = mtl_node->Attrib("opacity");
				if (attr)
				{
					mtl.albedo.w() = mtl_node->Attrib("opacity")->ValueFloat();
				}
			}

			XMLNodePtr metalness_node = mtl_node->FirstNode("metalness");
			if (metalness_node)
			{
				XMLAttributePtr attr = metalness_node->Attrib("value");
				if (attr)
				{
					mtl.metalness = attr->ValueFloat();
				}
				attr = metalness_node->Attrib("texture");
				if (attr)
				{
					mtl.tex_names[RenderMaterial::TS_Metalness] = std::string(attr->ValueString());
				}
			}

			XMLNodePtr glossiness_node = mtl_node->FirstNode("glossiness");
			if (glossiness_node)
			{
				XMLAttributePtr attr = glossiness_node->Attrib("value");
				if (attr)
				{
					mtl.glossiness = attr->ValueFloat();
				}
				attr = glossiness_node->Attrib("texture");
				if (attr)
				{
					mtl.tex_names[RenderMaterial::TS_Glossiness] = std::string(attr->ValueString());
				}
			}
			else
			{
				XMLAttributePtr attr = mtl_node->Attrib("shininess");
				if (attr)
				{
					float shininess = mtl_node->Attrib("shininess")->ValueFloat();
					shininess = MathLib::clamp(shininess, 1.0f, MAX_SHININESS);
					mtl.glossiness = Shininess2Glossiness(shininess);
				}
			}

			XMLNodePtr emissive_node = mtl_node->FirstNode("emissive");
			if (emissive_node)
			{
				XMLAttributePtr attr = emissive_node->Attrib("color");
				if (attr)
				{
					ExtractFVector<3>(attr->ValueString(), &mtl.emissive[0]);
				}
				attr = emissive_node->Attrib("texture");
				if (attr)
				{
					mtl.tex_names[RenderMaterial::TS_Emissive] = std::string(attr->ValueString());
				}
			}
			else
			{
				XMLAttributePtr attr = mtl_node->Attrib("emit");
				if (attr)
				{
					ExtractFVector<3>(attr->ValueString(), &mtl.emissive[0]);
				}
				else
				{
					attr = mtl_node->Attrib("emit_r");
					if (attr)
					{
						mtl.emissive.x() = attr->ValueFloat();
					}
					attr = mtl_node->Attrib("emit_g");
					if (attr)
					{
						mtl.emissive.y() = attr->ValueFloat();
					}
					attr = mtl_node->Attrib("emit_b");
					if (attr)
					{
						mtl.emissive.z() = attr->ValueFloat();
					}
				}
			}
			
			XMLNodePtr normal_node = mtl_node->FirstNode("normal");
			if (normal_node)
			{
				XMLAttributePtr attr = normal_node->Attrib("texture");
				if (attr)
				{
					mtl.tex_names[RenderMaterial::TS_Normal] = std::string(attr->ValueString());
				}
			}

			XMLNodePtr height_node = mtl_node->FirstNode("height");
			if (!height_node)
			{
				height_node = mtl_node->FirstNode("bump");
			}
			if (height_node)
			{
				XMLAttributePtr attr = height_node->Attrib("texture");
				if (attr)
				{
					mtl.tex_names[RenderMaterial::TS_Height] = std::string(attr->ValueString());
				}

				attr = height_node->Attrib("offset");
				if (attr)
				{
					mtl.height_offset_scale.x() = attr->ValueFloat();
				}

				attr = height_node->Attrib("scale");
				if (attr)
				{
					mtl.height_offset_scale.y() = attr->ValueFloat();
				}
			}

			XMLNodePtr detail_node = mtl_node->FirstNode("detail");
			if (detail_node)
			{
				XMLAttributePtr attr = detail_node->Attrib("mode");
				if (attr)
				{
					std::string_view const mode_str = attr->ValueString();
					size_t const mode_hash = HashRange(mode_str.begin(), mode_str.end());
					if (CT_HASH("Flat Tessellation") == mode_hash)
					{
						mtl.detail_mode = RenderMaterial::SDM_FlatTessellation;
					}
					else if (CT_HASH("Smooth Tessellation") == mode_hash)
					{
						mtl.detail_mode = RenderMaterial::SDM_SmoothTessellation;
					}
				}

				attr = detail_node->Attrib("height_offset");
				if (attr)
				{
					mtl.height_offset_scale.x() = attr->ValueFloat();
				}

				attr = detail_node->Attrib("height_scale");
				if (attr)
				{
					mtl.height_offset_scale.y() = attr->ValueFloat();
				}

				XMLNodePtr tess_node = detail_node->FirstNode("tess");
				if (tess_node)
				{
					attr = tess_node->Attrib("edge_hint");
					if (attr)
					{
						mtl.tess_factors.x() = attr->ValueFloat();
					}
					attr = tess_node->Attrib("inside_hint");
					if (attr)
					{
						mtl.tess_factors.y() = attr->ValueFloat();
					}
					attr = tess_node->Attrib("min");
					if (attr)
					{
						mtl.tess_factors.z() = attr->ValueFloat();
					}
					attr = tess_node->Attrib("max");
					if (attr)
					{
						mtl.tess_factors.w() = attr->ValueFloat();
					}
				}
				else
				{
					attr = detail_node->Attrib("edge_tess_hint");
					if (attr)
					{
						mtl.tess_factors.x() = attr->ValueFloat();
					}
					attr = detail_node->Attrib("inside_tess_hint");
					if (attr)
					{
						mtl.tess_factors.y() = attr->ValueFloat();
					}
					attr = detail_node->Attrib("min_tess");
					if (attr)
					{
						mtl.tess_factors.z() = attr->ValueFloat();
					}
					attr = detail_node->Attrib("max_tess");
					if (attr)
					{
						mtl.tess_factors.w() = attr->ValueFloat();
					}
				}
			}

			XMLNodePtr transparent_node = mtl_node->FirstNode("transparent");
			if (transparent_node)
			{
				XMLAttributePtr attr = transparent_node->Attrib("value");
				if (attr)
				{
					mtl.transparent = attr->ValueInt() ? true : false;
				}
			}

			XMLNodePtr alpha_test_node = mtl_node->FirstNode("alpha_test");
			if (alpha_test_node)
			{
				XMLAttributePtr attr = alpha_test_node->Attrib("value");
				if (attr)
				{
					mtl.alpha_test = attr->ValueFloat();
				}
			}

			XMLNodePtr sss_node = mtl_node->FirstNode("sss");
			if (sss_node)
			{
				XMLAttributePtr attr = sss_node->Attrib("value");
				if (attr)
				{
					mtl.sss = attr->ValueInt() ? true : false;
				}
			}
			else
			{
				XMLAttributePtr attr = mtl_node->Attrib("sss");
				if (attr)
				{
					mtl.sss = attr->ValueInt() ? true : false;
				}
			}

			XMLNodePtr two_sided_node = mtl_node->FirstNode("two_sided");
			if (two_sided_node)
			{
				XMLAttributePtr attr = two_sided_node->Attrib("value");
				if (attr)
				{
					mtl.two_sided = attr->ValueInt() ? true : false;
				}
			}

			XMLNodePtr tex_node = mtl_node->FirstNode("texture");
			if (!tex_node)
			{
				XMLNodePtr textures_chunk = mtl_node->FirstNode("textures_chunk");
				if (textures_chunk)
				{
					tex_node = textures_chunk->FirstNode("texture");
				}
			}
			if (tex_node)
			{
				for (; tex_node; tex_node = tex_node->NextSibling("texture"))
				{
					auto const type = tex_node->Attrib("type")->ValueString();
					size_t const type_hash = HashRange(type.begin(), type.end());

					std::string const name(tex_node->Attrib("name")->ValueString());

					if ((CT_HASH("Color") == type_hash) || (CT_HASH("Diffuse Color") == type_hash)
						|| (CT_HASH("Diffuse Color Map") == type_hash)
						|| (CT_HASH("Albedo") == type_hash))
					{
						mtl.tex_names[RenderMaterial::TS_Albedo] = name;
					}
					else if (CT_HASH("Metalness") == type_hash)
					{
						mtl.tex_names[RenderMaterial::TS_Metalness] = name;
					}
					else if ((CT_HASH("Glossiness") == type_hash) || (CT_HASH("Reflection Glossiness Map") == type_hash))
					{
						mtl.tex_names[RenderMaterial::TS_Glossiness] = name;
					}
					else if ((CT_HASH("Self-Illumination") == type_hash) || (CT_HASH("Emissive") == type_hash))
					{
						mtl.tex_names[RenderMaterial::TS_Emissive] = name;
					}
					else if ((CT_HASH("Normal") == type_hash) || (CT_HASH("Normal Map") == type_hash))
					{
						mtl.tex_names[RenderMaterial::TS_Normal] = name;
					}
					else if ((CT_HASH("Bump") == type_hash) || (CT_HASH("Bump Map") == type_hash)
						|| (CT_HASH("Height") == type_hash) || (CT_HASH("Height Map") == type_hash))
					{
						mtl.tex_names[RenderMaterial::TS_Height] = name;
					}
				}
			}
		}
	}

	void MeshConverter::CompileMeshBoundingBox(XMLNodePtr const & mesh_node, uint32_t mesh_index,
		bool& recompute_pos_bb, bool& recompute_tc_bb)
	{
		XMLNodePtr pos_bb_node = mesh_node->FirstNode("pos_bb");
		if (pos_bb_node)
		{
			float3 pos_min_bb, pos_max_bb;
			{
				XMLAttributePtr attr = pos_bb_node->Attrib("min");
				if (attr)
				{
					ExtractFVector<3>(attr->ValueString(), &pos_min_bb[0]);
				}
				else
				{
					XMLNodePtr pos_min_node = pos_bb_node->FirstNode("min");
					pos_min_bb.x() = pos_min_node->Attrib("x")->ValueFloat();
					pos_min_bb.y() = pos_min_node->Attrib("y")->ValueFloat();
					pos_min_bb.z() = pos_min_node->Attrib("z")->ValueFloat();
				}
			}
			{
				XMLAttributePtr attr = pos_bb_node->Attrib("max");
				if (attr)
				{
					ExtractFVector<3>(attr->ValueString(), &pos_max_bb[0]);
				}
				else
				{
					XMLNodePtr pos_max_node = pos_bb_node->FirstNode("max");
					pos_max_bb.x() = pos_max_node->Attrib("x")->ValueFloat();
					pos_max_bb.y() = pos_max_node->Attrib("y")->ValueFloat();
					pos_max_bb.z() = pos_max_node->Attrib("z")->ValueFloat();
				}
			}
			meshes_[mesh_index].pos_bb = AABBox(pos_min_bb, pos_max_bb);

			recompute_pos_bb = false;
		}
		else
		{
			recompute_pos_bb = true;
		}

		XMLNodePtr tc_bb_node = mesh_node->FirstNode("tc_bb");
		if (tc_bb_node)
		{
			float3 tc_min_bb, tc_max_bb;
			{
				XMLAttributePtr attr = tc_bb_node->Attrib("min");
				if (attr)
				{
					ExtractFVector<2>(attr->ValueString(), &tc_min_bb[0]);
				}
				else
				{
					XMLNodePtr tc_min_node = tc_bb_node->FirstNode("min");
					tc_min_bb.x() = tc_min_node->Attrib("x")->ValueFloat();
					tc_min_bb.y() = tc_min_node->Attrib("y")->ValueFloat();
				}
			}
			{
				XMLAttributePtr attr = tc_bb_node->Attrib("max");
				if (attr)
				{
					ExtractFVector<2>(attr->ValueString(), &tc_max_bb[0]);
				}
				else
				{
					XMLNodePtr tc_max_node = tc_bb_node->FirstNode("max");
					tc_max_bb.x() = tc_max_node->Attrib("x")->ValueFloat();
					tc_max_bb.y() = tc_max_node->Attrib("y")->ValueFloat();
				}
			}

			tc_min_bb.z() = 0;
			tc_max_bb.z() = 0;
			meshes_[mesh_index].tc_bb = AABBox(tc_min_bb, tc_max_bb);

			recompute_tc_bb = false;
		}
		else
		{
			recompute_tc_bb = true;
		}
	}

	void MeshConverter::CompileMeshesChunk(XMLNodePtr const & meshes_chunk)
	{
		uint32_t num_meshes = 0;
		for (XMLNodePtr mesh_node = meshes_chunk->FirstNode("mesh"); mesh_node; mesh_node = mesh_node->NextSibling("mesh"))
		{
			++ num_meshes;
		}
		meshes_.resize(num_meshes);
		nodes_.resize(num_meshes);

		uint32_t mesh_index = 0;
		for (XMLNodePtr mesh_node = meshes_chunk->FirstNode("mesh"); mesh_node; mesh_node = mesh_node->NextSibling("mesh"), ++ mesh_index)
		{
			nodes_[mesh_index].name = std::string(mesh_node->Attrib("name")->ValueString());
			nodes_[mesh_index].mesh_indices.push_back(mesh_index);

			meshes_[mesh_index].name = nodes_[mesh_index].name;
			meshes_[mesh_index].mtl_id = mesh_node->Attrib("mtl_id")->ValueInt();

			bool recompute_pos_bb;
			bool recompute_tc_bb;
			this->CompileMeshBoundingBox(mesh_node, mesh_index, recompute_pos_bb, recompute_tc_bb);
			if (recompute_pos_bb && recompute_tc_bb)
			{
				XMLNodePtr vertices_chunk = mesh_node->FirstNode("vertices_chunk");
				if (vertices_chunk)
				{
					this->CompileMeshBoundingBox(vertices_chunk, mesh_index, recompute_pos_bb, recompute_tc_bb);
				}
			}

			XMLNodePtr lod_node = mesh_node->FirstNode("lod");
			if (lod_node)
			{
				uint32_t mesh_lod = 0;
				for (; lod_node; lod_node = lod_node->NextSibling("lod"))
				{
					++ mesh_lod;
				}

				std::vector<XMLNodePtr> lod_nodes(mesh_lod);
				for (lod_node = mesh_node->FirstNode("lod"); lod_node; lod_node = lod_node->NextSibling("lod"))
				{
					uint32_t const lod = lod_node->Attrib("value")->ValueUInt();
					lod_nodes[lod] = lod_node;
				}

				meshes_[mesh_index].lods.resize(mesh_lod);
				nodes_[mesh_index].lod_transforms.assign(mesh_lod, float4x4::Identity());

				for (uint32_t lod = 0; lod < mesh_lod; ++ lod)
				{
					this->CompileMeshLodChunk(lod_nodes[lod], mesh_index, lod, recompute_pos_bb, recompute_tc_bb);

					recompute_pos_bb = false;
					recompute_tc_bb = false;
				}
			}
			else
			{
				meshes_[mesh_index].lods.resize(1);
				nodes_[mesh_index].lod_transforms.assign(1, float4x4::Identity());

				this->CompileMeshLodChunk(mesh_node, mesh_index, 0, recompute_pos_bb, recompute_tc_bb);

				recompute_pos_bb = false;
				recompute_tc_bb = false;
			}
		}
	}

	void MeshConverter::CompileMeshLodChunk(XMLNodePtr const & lod_node, uint32_t mesh_index, uint32_t lod,
		bool recompute_pos_bb, bool recompute_tc_bb)
	{
		XMLNodePtr vertices_chunk = lod_node->FirstNode("vertices_chunk");
		if (vertices_chunk)
		{
			this->CompileMeshesVerticesChunk(vertices_chunk, mesh_index, lod,
				recompute_pos_bb, recompute_tc_bb);
		}

		std::vector<uint8_t> triangle_indices;

		XMLNodePtr triangles_chunk = lod_node->FirstNode("triangles_chunk");
		if (triangles_chunk)
		{
			CompileMeshesTrianglesChunk(triangles_chunk, mesh_index, lod);
		}
	}

	void MeshConverter::CompileMeshesVerticesChunk(XMLNodePtr const & vertices_chunk, uint32_t mesh_index, uint32_t lod,
		bool recompute_pos_bb, bool recompute_tc_bb)
	{
		auto& mesh = meshes_[mesh_index];
		auto& mesh_lod = mesh.lods[lod];

		std::vector<float4> mesh_tangents;
		std::vector<float3> mesh_binormals;

		bool has_normal = false;
		bool has_diffuse = false;
		bool has_specular = false;
		bool has_tex_coord = false;
		bool has_tangent = false;
		bool has_binormal = false;
		bool has_tangent_quat = false;

		for (XMLNodePtr vertex_node = vertices_chunk->FirstNode("vertex"); vertex_node; vertex_node = vertex_node->NextSibling("vertex"))
		{
			{
				float3 pos;
				XMLAttributePtr attr = vertex_node->Attrib("x");
				if (attr)
				{
					pos.x() = vertex_node->Attrib("x")->ValueFloat();
					pos.y() = vertex_node->Attrib("y")->ValueFloat();
					pos.z() = vertex_node->Attrib("z")->ValueFloat();

					attr = vertex_node->Attrib("u");
					if (attr)
					{
						float3 tex_coord;
						tex_coord.x() = vertex_node->Attrib("u")->ValueFloat();
						tex_coord.y() = vertex_node->Attrib("v")->ValueFloat();
						tex_coord.z() = 0;
						mesh_lod.texcoords[0].push_back(tex_coord);
					}
				}
				else
				{
					ExtractFVector<3>(vertex_node->Attrib("v")->ValueString(), &pos[0]);
				}
				mesh_lod.positions.push_back(pos);
			}

			XMLNodePtr diffuse_node = vertex_node->FirstNode("diffuse");
			if (diffuse_node)
			{
				has_diffuse = true;

				float4 diffuse;
				XMLAttributePtr attr = diffuse_node->Attrib("v");
				if (attr)
				{
					ExtractFVector<4>(attr->ValueString(), &diffuse[0]);
				}
				else
				{
					diffuse.x() = diffuse_node->Attrib("r")->ValueFloat();
					diffuse.y() = diffuse_node->Attrib("g")->ValueFloat();
					diffuse.z() = diffuse_node->Attrib("b")->ValueFloat();
					diffuse.w() = diffuse_node->Attrib("a")->ValueFloat();										
				}
				mesh_lod.diffuses.push_back(Color(diffuse.x(), diffuse.y(), diffuse.z(), diffuse.w()));
			}

			XMLNodePtr specular_node = vertex_node->FirstNode("specular");
			if (specular_node)
			{
				has_specular = true;

				float3 specular;
				XMLAttributePtr attr = specular_node->Attrib("v");
				if (attr)
				{
					ExtractFVector<3>(attr->ValueString(), &specular[0]);
				}
				else
				{
					specular.x() = specular_node->Attrib("r")->ValueFloat();
					specular.y() = specular_node->Attrib("g")->ValueFloat();
					specular.z() = specular_node->Attrib("b")->ValueFloat();
				}
				mesh_lod.speculars.push_back(Color(specular.x(), specular.y(), specular.z(), 1));
			}

			if (!vertex_node->Attrib("u"))
			{
				XMLNodePtr tex_coord_node = vertex_node->FirstNode("tex_coord");
				if (tex_coord_node)
				{
					has_tex_coord = true;

					float3 tex_coord;
					XMLAttributePtr attr = tex_coord_node->Attrib("u");
					if (attr)
					{
						tex_coord.x() = tex_coord_node->Attrib("u")->ValueFloat();
						tex_coord.y() = tex_coord_node->Attrib("v")->ValueFloat();
					}
					else
					{
						ExtractFVector<2>(tex_coord_node->Attrib("v")->ValueString(), &tex_coord[0]);
					}
					tex_coord.z() = 0;
					mesh_lod.texcoords[0].push_back(tex_coord);
				}
			}

			XMLNodePtr weight_node = vertex_node->FirstNode("weight");
			if (weight_node)
			{
				std::vector<std::pair<uint32_t, float>> binding;

				XMLAttributePtr attr = weight_node->Attrib("joint");
				if (!attr)
				{
					attr = weight_node->Attrib("bone_index");
				}
				if (attr)
				{
					XMLAttributePtr weight_attr = weight_node->Attrib("weight");

					std::string_view const index_str = attr->ValueString();
					std::string_view const weight_str = weight_attr->ValueString();
					std::vector<std::string> index_strs;
					std::vector<std::string> weight_strs;
					boost::algorithm::split(index_strs, index_str, boost::is_any_of(" "));
					boost::algorithm::split(weight_strs, weight_str, boost::is_any_of(" "));
					
					for (size_t num_blend = 0; num_blend < index_strs.size(); ++ num_blend)
					{
						binding.push_back({ static_cast<uint32_t>(atoi(index_strs[num_blend].c_str())),
							static_cast<float>(atof(weight_strs[num_blend].c_str())) });
					}
				}
				else
				{
					while (weight_node)
					{
						binding.push_back({ weight_node->Attrib("bone_index")->ValueUInt(),
							weight_node->Attrib("weight")->ValueFloat() });

						weight_node = weight_node->NextSibling("weight");
					}
				}

				mesh_lod.joint_bindings.emplace_back(std::move(binding));
			}
						
			XMLNodePtr normal_node = vertex_node->FirstNode("normal");
			if (normal_node)
			{
				has_normal = true;

				float3 normal;
				XMLAttributePtr attr = normal_node->Attrib("v");
				if (attr)
				{
					ExtractFVector<3>(attr->ValueString(), &normal[0]);
				}
				else
				{
					normal.x() = normal_node->Attrib("x")->ValueFloat();
					normal.y() = normal_node->Attrib("y")->ValueFloat();
					normal.z() = normal_node->Attrib("z")->ValueFloat();
				}
				mesh_lod.normals.push_back(normal);
			}

			XMLNodePtr tangent_node = vertex_node->FirstNode("tangent");
			if (tangent_node)
			{
				has_tangent = true;

				float4 tangent;
				XMLAttributePtr attr = tangent_node->Attrib("v");
				if (attr)
				{
					ExtractFVector<4>(attr->ValueString(), &tangent[0]);
				}
				else
				{
					tangent.x() = tangent_node->Attrib("x")->ValueFloat();
					tangent.y() = tangent_node->Attrib("y")->ValueFloat();
					tangent.z() = tangent_node->Attrib("z")->ValueFloat();
					attr = tangent_node->Attrib("w");
					if (attr)
					{
						tangent.w() = attr->ValueFloat();
					}
					else
					{
						tangent.w() = 1;
					}
				}
				mesh_tangents.push_back(tangent);
			}

			XMLNodePtr binormal_node = vertex_node->FirstNode("binormal");
			if (binormal_node)
			{
				has_binormal = true;

				float3 binormal;
				XMLAttributePtr attr = binormal_node->Attrib("v");
				if (attr)
				{
					ExtractFVector<3>(attr->ValueString(), &binormal[0]);
				}
				else
				{
					binormal.x() = binormal_node->Attrib("x")->ValueFloat();
					binormal.y() = binormal_node->Attrib("y")->ValueFloat();
					binormal.z() = binormal_node->Attrib("z")->ValueFloat();
				}
				mesh_binormals.push_back(binormal);
			}

			XMLNodePtr tangent_quat_node = vertex_node->FirstNode("tangent_quat");
			if (tangent_quat_node)
			{
				has_tangent_quat = true;

				Quaternion tangent_quat;
				XMLAttributePtr const & attr = tangent_quat_node->Attrib("v");
				if (attr)
				{
					ExtractFVector<4>(attr->ValueString(), &tangent_quat[0]);
				}
				else
				{
					tangent_quat.x() = tangent_quat_node->Attrib("x")->ValueFloat();
					tangent_quat.y() = tangent_quat_node->Attrib("y")->ValueFloat();
					tangent_quat.z() = tangent_quat_node->Attrib("z")->ValueFloat();
					tangent_quat.w() = tangent_quat_node->Attrib("w")->ValueFloat();
				}

				float3 const tangent = MathLib::transform_quat(float3(1, 0, 0), tangent_quat);
				float3 const binormal = MathLib::transform_quat(float3(0, 1, 0), tangent_quat) * MathLib::sgn(tangent_quat.w());
				float3 const normal = MathLib::transform_quat(float3(0, 0, 1), tangent_quat);

				mesh_lod.tangents.push_back(tangent);
				mesh_lod.binormals.push_back(binormal);
				mesh_lod.normals.push_back(normal);
			}
		}

		bool recompute_tangent_quat = false;

		{
			if (has_diffuse)
			{
				has_diffuse_ = true;
			}

			if (has_specular)
			{
				has_specular_ = true;
			}

			if (has_tex_coord)
			{
				has_texcoord_ = true;
				mesh.has_texcoord[0] = true;
			}
			else
			{
				mesh.has_texcoord[0] = false;
			}

			if (has_tangent_quat)
			{
				has_tangent_quat_ = true;
			}
			else
			{
				if (has_normal && !has_tangent && !has_binormal)
				{
					has_normal_ = true;
					mesh.has_normal = true;
				}
				else
				{
					mesh.has_normal = false;

					if ((has_normal && has_tangent) || (has_normal && has_binormal)
						|| (has_tangent && has_binormal))
					{
						has_tangent_quat_ = true;
						mesh.has_tangent_frame = true;

						if (!has_tangent_quat)
						{
							recompute_tangent_quat = true;
						}
					}
					else
					{
						mesh.has_tangent_frame = false;
					}
				}
			}
		}

		if (recompute_pos_bb && (lod == 0))
		{
			mesh.pos_bb = MathLib::compute_aabbox(mesh_lod.positions.begin(), mesh_lod.positions.end());
		}
		if (recompute_tc_bb && (lod == 0))
		{
			mesh.tc_bb = MathLib::compute_aabbox(mesh_lod.texcoords[0].begin(), mesh_lod.texcoords[0].end());
		}
		if (recompute_tangent_quat)
		{
			mesh_lod.tangents.resize(mesh_lod.positions.size());
			mesh_lod.binormals.resize(mesh_lod.positions.size());
			mesh_lod.normals.resize(mesh_lod.positions.size());
			for (uint32_t index = 0; index < mesh_lod.positions.size(); ++ index)
			{
				float3 tangent, binormal, normal;
				if (has_tangent)
				{
					tangent = float3(mesh_tangents[index].x(), mesh_tangents[index].y(),
						mesh_tangents[index].z());
				}
				if (has_binormal)
				{
					binormal = mesh_binormals[index];
				}
				if (has_normal)
				{
					normal = mesh_lod.normals[index];
				}

				if (!has_tangent)
				{
					BOOST_ASSERT(has_binormal && has_normal);

					tangent = MathLib::cross(binormal, normal);
				}
				if (!has_binormal)
				{
					BOOST_ASSERT(has_tangent && has_normal);

					binormal = MathLib::cross(normal, tangent) * mesh_tangents[index].w();
				}
				if (!has_normal)
				{
					BOOST_ASSERT(has_tangent && has_binormal);

					normal = MathLib::cross(tangent, binormal);
				}

				mesh_lod.tangents[index] = tangent;
				mesh_lod.binormals[index] = binormal;
				mesh_lod.normals[index] = normal;
			}
		}
	}

	void MeshConverter::CompileMeshesTrianglesChunk(XMLNodePtr const & triangles_chunk, uint32_t mesh_index, uint32_t lod)
	{
		auto& mesh = meshes_[mesh_index];
		auto& mesh_lod = mesh.lods[lod];

		for (XMLNodePtr tri_node = triangles_chunk->FirstNode("triangle"); tri_node; tri_node = tri_node->NextSibling("triangle"))
		{
			uint32_t ind[3];
			XMLAttributePtr attr = tri_node->Attrib("index");
			if (attr)
			{
				ExtractUIVector<3>(attr->ValueString(), &ind[0]);
			}
			else
			{
				ind[0] = tri_node->Attrib("a")->ValueUInt();
				ind[1] = tri_node->Attrib("b")->ValueUInt();
				ind[2] = tri_node->Attrib("c")->ValueUInt();
			}
			mesh_lod.indices.push_back(ind[0]);
			mesh_lod.indices.push_back(ind[1]);
			mesh_lod.indices.push_back(ind[2]);
		}
	}

	void MeshConverter::CompileBonesChunk(XMLNodePtr const & bones_chunk)
	{
		for (XMLNodePtr bone_node = bones_chunk->FirstNode("bone"); bone_node; bone_node = bone_node->NextSibling("bone"))
		{
			Joint joint;

			joint.name = std::string(bone_node->Attrib("name")->ValueString());
			joint.parent = static_cast<int16_t>(bone_node->Attrib("parent")->ValueInt());

			XMLNodePtr bind_pos_node = bone_node->FirstNode("bind_pos");
			if (bind_pos_node)
			{
				float3 bind_pos(bind_pos_node->Attrib("x")->ValueFloat(), bind_pos_node->Attrib("y")->ValueFloat(),
					bind_pos_node->Attrib("z")->ValueFloat());

				XMLNodePtr bind_quat_node = bone_node->FirstNode("bind_quat");
				Quaternion bind_quat(bind_quat_node->Attrib("x")->ValueFloat(), bind_quat_node->Attrib("y")->ValueFloat(),
					bind_quat_node->Attrib("z")->ValueFloat(), bind_quat_node->Attrib("w")->ValueFloat());

				float scale = MathLib::length(bind_quat);
				bind_quat /= scale;

				joint.bind_dual = MathLib::quat_trans_to_udq(bind_quat, bind_pos);
				joint.bind_real = bind_quat * scale;
				joint.bind_scale = scale;
			}
			else
			{
				XMLNodePtr bind_real_node = bone_node->FirstNode("real");
				if (!bind_real_node)
				{
					bind_real_node = bone_node->FirstNode("bind_real");
				}
				XMLAttributePtr attr = bind_real_node->Attrib("v");
				if (attr)
				{
					ExtractFVector<4>(attr->ValueString(), &joint.bind_real[0]);
				}
				else
				{
					joint.bind_real.x() = bind_real_node->Attrib("x")->ValueFloat();
					joint.bind_real.y() = bind_real_node->Attrib("y")->ValueFloat();
					joint.bind_real.z() = bind_real_node->Attrib("z")->ValueFloat();
					joint.bind_real.w() = bind_real_node->Attrib("w")->ValueFloat();
				}

				XMLNodePtr bind_dual_node = bone_node->FirstNode("dual");
				if (!bind_dual_node)
				{
					bind_dual_node = bone_node->FirstNode("bind_dual");
				}
				attr = bind_dual_node->Attrib("v");
				if (attr)
				{
					ExtractFVector<4>(attr->ValueString(), &joint.bind_dual[0]);
				}
				else
				{
					joint.bind_dual.x() = bind_dual_node->Attrib("x")->ValueFloat();
					joint.bind_dual.y() = bind_dual_node->Attrib("y")->ValueFloat();
					joint.bind_dual.z() = bind_dual_node->Attrib("z")->ValueFloat();
					joint.bind_dual.w() = bind_dual_node->Attrib("w")->ValueFloat();
				}

				joint.bind_scale = MathLib::length(joint.bind_real);
				joint.bind_real /= joint.bind_scale;
				if (MathLib::SignBit(joint.bind_real.w()) < 0)
				{
					joint.bind_real = -joint.bind_real;
					joint.bind_scale = -joint.bind_scale;
				}
			}

			std::tie(joint.inverse_origin_real, joint.inverse_origin_dual) = MathLib::inverse(joint.bind_real, joint.bind_dual);
			joint.inverse_origin_scale = 1 / joint.bind_scale;

			joints_.emplace_back(std::move(joint));
		}
	}

	void MeshConverter::CompileKeyFramesChunk(XMLNodePtr const & key_frames_chunk)
	{
		auto& skinned_model = *checked_pointer_cast<SkinnedModel>(render_model_);

		XMLAttributePtr nf_attr = key_frames_chunk->Attrib("num_frames");
		if (nf_attr)
		{
			skinned_model.NumFrames(nf_attr->ValueUInt());
		}
		else
		{
			int32_t start_frame = key_frames_chunk->Attrib("start_frame")->ValueInt();
			int32_t end_frame = key_frames_chunk->Attrib("end_frame")->ValueInt();
			skinned_model.NumFrames(end_frame - start_frame);
		}
		skinned_model.FrameRate(key_frames_chunk->Attrib("frame_rate")->ValueUInt());

		auto kfss = MakeSharedPtr<std::vector<KeyFrameSet>>();
		kfss->resize(joints_.size());
		uint32_t joint_id = 0;
		for (XMLNodePtr kf_node = key_frames_chunk->FirstNode("key_frame"); kf_node; kf_node = kf_node->NextSibling("key_frame"))
		{
			XMLAttributePtr joint_attr = kf_node->Attrib("joint");
			if (joint_attr)
			{
				joint_id = joint_attr->ValueUInt();
			}
			else
			{
				++ joint_id;
			}
			KeyFrameSet& kfs = (*kfss)[joint_id];

			int32_t frame_id = -1;
			for (XMLNodePtr key_node = kf_node->FirstNode("key"); key_node; key_node = key_node->NextSibling("key"))
			{
				XMLAttributePtr id_attr = key_node->Attrib("id");
				if (id_attr)
				{
					frame_id = id_attr->ValueInt();
				}
				else
				{
					++ frame_id;
				}
				kfs.frame_id.push_back(frame_id);

				Quaternion bind_real, bind_dual;
				float bind_scale;
				XMLNodePtr pos_node = key_node->FirstNode("pos");
				if (pos_node)
				{
					float3 bind_pos(pos_node->Attrib("x")->ValueFloat(), pos_node->Attrib("y")->ValueFloat(),
						pos_node->Attrib("z")->ValueFloat());

					XMLNodePtr quat_node = key_node->FirstNode("quat");
					bind_real = Quaternion(quat_node->Attrib("x")->ValueFloat(), quat_node->Attrib("y")->ValueFloat(),
						quat_node->Attrib("z")->ValueFloat(), quat_node->Attrib("w")->ValueFloat());

					bind_scale = MathLib::length(bind_real);
					bind_real /= bind_scale;

					bind_dual = MathLib::quat_trans_to_udq(bind_real, bind_pos);
				}
				else
				{
					XMLNodePtr bind_real_node = key_node->FirstNode("real");
					if (!bind_real_node)
					{
						bind_real_node = key_node->FirstNode("bind_real");
					}
					XMLAttributePtr attr = bind_real_node->Attrib("v");
					if (attr)
					{
						ExtractFVector<4>(attr->ValueString(), &bind_real[0]);
					}
					else
					{
						bind_real.x() = bind_real_node->Attrib("x")->ValueFloat();
						bind_real.y() = bind_real_node->Attrib("y")->ValueFloat();
						bind_real.z() = bind_real_node->Attrib("z")->ValueFloat();
						bind_real.w() = bind_real_node->Attrib("w")->ValueFloat();
					}

					XMLNodePtr bind_dual_node = key_node->FirstNode("dual");
					if (!bind_dual_node)
					{
						bind_dual_node = key_node->FirstNode("bind_dual");
					}
					attr = bind_dual_node->Attrib("v");
					if (attr)
					{
						ExtractFVector<4>(attr->ValueString(), &bind_dual[0]);
					}
					else
					{
						bind_dual.x() = bind_dual_node->Attrib("x")->ValueFloat();
						bind_dual.y() = bind_dual_node->Attrib("y")->ValueFloat();
						bind_dual.z() = bind_dual_node->Attrib("z")->ValueFloat();
						bind_dual.w() = bind_dual_node->Attrib("w")->ValueFloat();
					}

					bind_scale = MathLib::length(bind_real);
					bind_real /= bind_scale;
					if (MathLib::SignBit(bind_real.w()) < 0)
					{
						bind_real = -bind_real;
						bind_scale = -bind_scale;
					}
				}

				kfs.bind_real.push_back(bind_real);
				kfs.bind_dual.push_back(bind_dual);
				kfs.bind_scale.push_back(bind_scale);
			}

			this->CompressKeyFrameSet(kfs);
		}

		skinned_model.AttachKeyFrameSets(kfss);
	}

	void MeshConverter::CompileBBKeyFramesChunk(XMLNodePtr const & bb_kfs_chunk, uint32_t mesh_index)
	{
		auto& skinned_model = *checked_pointer_cast<SkinnedModel>(render_model_);
		auto& skinned_mesh = *checked_pointer_cast<SkinnedMesh>(skinned_model.Subrenderable(mesh_index));

		auto bb_kfs = MakeSharedPtr<AABBKeyFrameSet>();
		if (bb_kfs_chunk)
		{
			for (XMLNodePtr bb_kf_node = bb_kfs_chunk->FirstNode("bb_key_frame"); bb_kf_node;
				bb_kf_node = bb_kf_node->NextSibling("bb_key_frame"))
			{
				bb_kfs->frame_id.clear();
				bb_kfs->bb.clear();

				int32_t frame_id = -1;
				for (XMLNodePtr key_node = bb_kf_node->FirstNode("key"); key_node; key_node = key_node->NextSibling("key"))
				{
					XMLAttributePtr id_attr = key_node->Attrib("id");
					if (id_attr)
					{
						frame_id = id_attr->ValueInt();
					}
					else
					{
						++ frame_id;
					}
					bb_kfs->frame_id.push_back(frame_id);

					float3 bb_min, bb_max;
					XMLAttributePtr attr = key_node->Attrib("min");
					if (attr)
					{
						ExtractFVector<3>(attr->ValueString(), &bb_min[0]);
					}
					else
					{
						XMLNodePtr min_node = key_node->FirstNode("min");
						bb_min.x() = min_node->Attrib("x")->ValueFloat();
						bb_min.y() = min_node->Attrib("y")->ValueFloat();
						bb_min.z() = min_node->Attrib("z")->ValueFloat();
					}
					attr = key_node->Attrib("max");
					if (attr)
					{
						ExtractFVector<3>(attr->ValueString(), &bb_max[0]);
					}
					else
					{
						XMLNodePtr max_node = key_node->FirstNode("max");
						bb_max.x() = max_node->Attrib("x")->ValueFloat();
						bb_max.y() = max_node->Attrib("y")->ValueFloat();
						bb_max.z() = max_node->Attrib("z")->ValueFloat();
					}

					bb_kfs->bb.push_back(AABBox(bb_min, bb_max));
				}
			}
		}
		else
		{
			bb_kfs->frame_id.resize(2);
			bb_kfs->bb.resize(2);

			bb_kfs->frame_id[0] = 0;
			bb_kfs->frame_id[1] = skinned_model.NumFrames() - 1;

			bb_kfs->bb[0] = bb_kfs->bb[1] = skinned_mesh.PosBound();
		}

		skinned_mesh.AttachFramePosBounds(bb_kfs);
	}

	void MeshConverter::CompileActionsChunk(XMLNodePtr const & actions_chunk)
	{
		auto& skinned_model = *checked_pointer_cast<SkinnedModel>(render_model_);

		XMLNodePtr action_node;
		if (actions_chunk)
		{
			action_node = actions_chunk->FirstNode("action");
		}

		auto actions = MakeSharedPtr<std::vector<AnimationAction>>();

		AnimationAction action;
		if (action_node)
		{
			for (; action_node; action_node = action_node->NextSibling("action"))
			{
				action.name = std::string(action_node->Attrib("name")->ValueString());

				action.start_frame = action_node->Attrib("start")->ValueUInt();
				action.end_frame = action_node->Attrib("end")->ValueUInt();

				actions->push_back(action);
			}
		}
		else
		{
			action.name = "root";
			action.start_frame = 0;
			action.end_frame = skinned_model.NumFrames();

			actions->push_back(action);
		}

		skinned_model.AttachActions(actions);
	}

	void MeshConverter::LoadFromMeshML(std::string const & input_name, MeshMetadata const & metadata)
	{
		KFL_UNUSED(metadata);

		ResIdentifierPtr file = ResLoader::Instance().Open(input_name);
		KlayGE::XMLDocument doc;
		XMLNodePtr root = doc.Parse(file);

		BOOST_ASSERT(root->Attrib("version") && (root->Attrib("version")->ValueInt() >= 1));

		XMLNodePtr bones_chunk = root->FirstNode("bones_chunk");
		if (bones_chunk)
		{
			this->CompileBonesChunk(bones_chunk);
		}

		bool const skinned = !joints_.empty();

		if (skinned)
		{
			render_model_ = MakeSharedPtr<SkinnedModel>(L"Software");
		}
		else
		{
			render_model_ = MakeSharedPtr<RenderModel>(L"Software");
		}

		XMLNodePtr materials_chunk = root->FirstNode("materials_chunk");
		if (materials_chunk)
		{
			this->CompileMaterialsChunk(materials_chunk);
		}

		XMLNodePtr meshes_chunk = root->FirstNode("meshes_chunk");
		if (meshes_chunk)
		{
			this->CompileMeshesChunk(meshes_chunk);
		}

		XMLNodePtr key_frames_chunk = root->FirstNode("key_frames_chunk");
		if (key_frames_chunk)
		{
			this->CompileKeyFramesChunk(key_frames_chunk);

			auto& skinned_model = *checked_pointer_cast<SkinnedModel>(render_model_);
			auto& kfs = *skinned_model.GetKeyFrameSets();

			for (size_t i = 0; i < kfs.size(); ++ i)
			{
				auto& kf = kfs[i];
				if (kf.frame_id.empty())
				{
					Quaternion inv_parent_real;
					Quaternion inv_parent_dual;
					float inv_parent_scale;
					if (joints_[i].parent < 0)
					{
						inv_parent_real = Quaternion::Identity();
						inv_parent_dual = Quaternion(0, 0, 0, 0);
						inv_parent_scale = 1;
					}
					else
					{
						std::tie(inv_parent_real, inv_parent_dual)
							= MathLib::inverse(joints_[joints_[i].parent].bind_real, joints_[joints_[i].parent].bind_dual);
						inv_parent_scale = 1 / joints_[joints_[i].parent].bind_scale;
					}

					kf.frame_id.push_back(0);
					kf.bind_real.push_back(MathLib::mul_real(joints_[i].bind_real, inv_parent_real));
					kf.bind_dual.push_back(MathLib::mul_dual(joints_[i].bind_real, joints_[i].bind_dual * inv_parent_scale,
						inv_parent_real, inv_parent_dual));
					kf.bind_scale.push_back(joints_[i].bind_scale * inv_parent_scale);
				}
			}

			XMLNodePtr bb_kfs_chunk = root->FirstNode("bb_key_frames_chunk");
			for (uint32_t mesh_index = 0; mesh_index < skinned_model.NumSubrenderables(); ++ mesh_index)
			{
				this->CompileBBKeyFramesChunk(bb_kfs_chunk, mesh_index);
			}
		}

		XMLNodePtr actions_chunk = root->FirstNode("actions_chunk");
		if (actions_chunk)
		{
			this->CompileActionsChunk(actions_chunk);
		}
	}

	void MeshConverter::RemoveUnusedJoints()
	{
		std::vector<uint32_t> joint_mapping(joints_.size());
		std::vector<bool> joints_used(joints_.size(), false);

		for (auto const & mesh : meshes_)
		{
			for (auto const & lod : mesh.lods)
			{
				for (auto const & bindings : lod.joint_bindings)
				{
					for (auto const & bind : bindings)
					{
						joints_used[bind.first] = true;
					}
				}
			}
		}

		for (uint32_t ji = 0; ji < joints_.size(); ++ ji)
		{
			if (joints_used[ji])
			{
				Joint const * j = &joints_[ji];
				while ((j->parent != -1) && !joints_used[j->parent])
				{
					joints_used[j->parent] = true;
					j = &joints_[j->parent];
				}
			}
		}

		uint32_t new_joint_id = 0;
		for (uint32_t ji = 0; ji < joints_.size(); ++ ji)
		{
			if (joints_used[ji])
			{
				joint_mapping[ji] = new_joint_id;
				++ new_joint_id;
			}
		}

		auto& skinned_model = *checked_pointer_cast<SkinnedModel>(render_model_);
		auto& kfs = *skinned_model.GetKeyFrameSets();

		for (uint32_t ji = 0; ji < joints_.size(); ++ ji)
		{
			if (joints_used[ji])
			{
				BOOST_ASSERT(joint_mapping[ji] <= ji);
				joints_[joint_mapping[ji]] = joints_[ji];
				kfs[joint_mapping[ji]] = kfs[ji];
			}
		}
		joints_.resize(new_joint_id);
		kfs.resize(joints_.size());

		for (auto& mesh : meshes_)
		{
			for (auto& lod : mesh.lods)
			{
				for (auto& bindings : lod.joint_bindings)
				{
					for (auto& bind : bindings)
					{
						bind.first = joint_mapping[bind.first];
					}
				}
			}
		}
	}

	void MeshConverter::RemoveUnusedMaterials()
	{
		std::vector<uint32_t> mtl_mapping(render_model_->NumMaterials());
		std::vector<bool> mtl_used(mtl_mapping.size(), false);

		for (auto& mesh : meshes_)
		{
			mtl_used[mesh.mtl_id] = true;
		}

		uint32_t new_mtl_id = 0;
		for (uint32_t i = 0; i < render_model_->NumMaterials(); ++ i)
		{
			if (mtl_used[i])
			{
				mtl_mapping[i] = new_mtl_id;
				++ new_mtl_id;
			}
		}

		for (uint32_t i = 0; i < render_model_->NumMaterials(); ++ i)
		{
			BOOST_ASSERT(mtl_mapping[i] <= i);
			render_model_->GetMaterial(i) = render_model_->GetMaterial(mtl_mapping[i]);
		}
		render_model_->NumMaterials(new_mtl_id);

		for (auto& mesh : meshes_)
		{
			mesh.mtl_id = mtl_mapping[mesh.mtl_id];
		}
	}

	void MeshConverter::CompressKeyFrameSet(KeyFrameSet& kf)
	{
		float const THRESHOLD = 1e-3f;

		BOOST_ASSERT((kf.bind_real.size() == kf.bind_dual.size())
			&& (kf.frame_id.size() == kf.bind_scale.size())
			&& (kf.frame_id.size() == kf.bind_real.size()));

		int base = 0;
		while (base < static_cast<int>(kf.frame_id.size() - 2))
		{
			int const frame0 = kf.frame_id[base + 0];
			int const frame1 = kf.frame_id[base + 1];
			int const frame2 = kf.frame_id[base + 2];
			float const factor = static_cast<float>(frame1 - frame0) / (frame2 - frame0);
			Quaternion interpolate_real;
			Quaternion interpolate_dual;
			std::tie(interpolate_real, interpolate_dual) = MathLib::sclerp(kf.bind_real[base + 0], kf.bind_dual[base + 0],
				kf.bind_real[base + 2], kf.bind_dual[base + 2], factor);
			float const scale = MathLib::lerp(kf.bind_scale[base + 0], kf.bind_scale[base + 2], factor);

			if (MathLib::dot(kf.bind_real[base + 1], interpolate_real) < 0)
			{
				interpolate_real = -interpolate_real;
				interpolate_dual = -interpolate_dual;
			}

			Quaternion diff_real;
			Quaternion diff_dual;
			std::tie(diff_real, diff_dual) = MathLib::inverse(kf.bind_real[base + 1], kf.bind_dual[base + 1]);
			diff_dual = MathLib::mul_dual(diff_real, diff_dual * scale, interpolate_real, interpolate_dual);
			diff_real = MathLib::mul_real(diff_real, interpolate_real);
			float diff_scale = scale * kf.bind_scale[base + 1];

			if ((MathLib::abs(diff_real.x()) < THRESHOLD) && (MathLib::abs(diff_real.y()) < THRESHOLD)
				&& (MathLib::abs(diff_real.z()) < THRESHOLD) && (MathLib::abs(diff_real.w() - 1) < THRESHOLD)
				&& (MathLib::abs(diff_dual.x()) < THRESHOLD) && (MathLib::abs(diff_dual.y()) < THRESHOLD)
				&& (MathLib::abs(diff_dual.z()) < THRESHOLD) && (MathLib::abs(diff_dual.w()) < THRESHOLD)
				&& (MathLib::abs(diff_scale - 1) < THRESHOLD))
			{
				kf.frame_id.erase(kf.frame_id.begin() + base + 1);
				kf.bind_real.erase(kf.bind_real.begin() + base + 1);
				kf.bind_dual.erase(kf.bind_dual.begin() + base + 1);
				kf.bind_scale.erase(kf.bind_scale.begin() + base + 1);
			}
			else
			{
				++ base;
			}
		}
	}


	RenderModelPtr MeshConverter::Convert(std::string_view input_name, MeshMetadata const & metadata)
	{
		std::string const input_name_str = ResLoader::Instance().Locate(input_name);
		if (input_name_str.empty())
		{
			LogError() << "Could NOT find " << input_name << '.' << std::endl;
			return RenderModelPtr();
		}

		std::filesystem::path input_path(input_name_str);
		auto const in_folder = input_path.parent_path().string();
		bool const in_path = ResLoader::Instance().IsInPath(in_folder);
		if (!in_path)
		{
			ResLoader::Instance().AddPath(in_folder);
		}

		render_model_.reset();
		meshes_.clear();
		nodes_.clear();
		joints_.clear();
		has_normal_ = false;
		has_tangent_quat_ = false;
		has_texcoord_ = false;
		has_diffuse_ = false;
		has_specular_ = false;

		auto const input_ext = input_path.extension();
		if (input_ext == ".model_bin")
		{
			render_model_ = LoadSoftwareModel(input_name_str);
			return render_model_;
		}
		else if (input_ext == ".meshml")
		{
			this->LoadFromMeshML(input_name_str, metadata);
		}
		else
		{
			this->LoadFromAssimp(input_name_str, metadata);
		}
		if (!render_model_)
		{
			return RenderModelPtr();
		}

		uint32_t const num_lods = static_cast<uint32_t>(meshes_[0].lods.size());
		bool const skinned = !joints_.empty();

		if (skinned)
		{
			this->RemoveUnusedJoints();
		}
		this->RemoveUnusedMaterials();

		auto global_transform = metadata.Transform();
		if (metadata.AutoCenter())
		{
			bool first_aabb = true;
			AABBox model_aabb;
			for (auto const & node : nodes_)
			{
				for (auto const mesh_index : node.mesh_indices)
				{
					auto trans_aabb = MathLib::transform_aabb(meshes_[mesh_index].pos_bb, node.lod_transforms[0]);
					if (first_aabb)
					{
						model_aabb = trans_aabb;
						first_aabb = false;
					}
					else
					{
						model_aabb |= trans_aabb;
					}
				}
			}

			global_transform = MathLib::translation(-model_aabb.Center()) * global_transform;
		}

		std::vector<VertexElement> merged_ves;
		std::vector<std::vector<uint8_t>> merged_vertices;
		std::vector<uint8_t> merged_indices;
		std::vector<uint32_t> mesh_num_vertices;
		std::vector<uint32_t> mesh_base_vertices(1, 0);
		std::vector<uint32_t> mesh_num_indices;
		std::vector<uint32_t> mesh_start_indices(1, 0);
		bool is_index_16_bit;

		int position_stream = -1;
		int normal_stream = -1;
		int tangent_quat_stream = -1;
		int diffuse_stream = -1;
		int specular_stream = -1;
		int texcoord_stream = -1;
		int blend_weights_stream = -1;
		int blend_indices_stream = -1;
		{
			int stream_index = 0;
			{
				merged_ves.push_back(VertexElement(VEU_Position, 0, EF_SIGNED_ABGR16));
				position_stream = stream_index;
			}
			if (has_tangent_quat_)
			{
				merged_ves.push_back(VertexElement(VEU_Tangent, 0, EF_ABGR8));
				++ stream_index;
				tangent_quat_stream = stream_index;
			}
			else if (has_normal_)
			{
				merged_ves.push_back(VertexElement(VEU_Normal, 0, EF_ABGR8));
				++ stream_index;
				normal_stream = stream_index;
			}
			if (has_diffuse_)
			{
				merged_ves.push_back(VertexElement(VEU_Diffuse, 0, EF_ABGR8));
				++ stream_index;
				diffuse_stream = stream_index;
			}
			if (has_specular_)
			{
				merged_ves.push_back(VertexElement(VEU_Specular, 0, EF_ABGR8));
				++ stream_index;
				specular_stream = stream_index;
			}
			if (has_texcoord_)
			{
				merged_ves.push_back(VertexElement(VEU_TextureCoord, 0, EF_SIGNED_GR16));
				++ stream_index;
				texcoord_stream = stream_index;
			}

			if (skinned)
			{
				merged_ves.push_back(VertexElement(VEU_BlendWeight, 0, EF_ABGR8));
				++ stream_index;
				blend_weights_stream = stream_index;

				merged_ves.push_back(VertexElement(VEU_BlendIndex, 0, EF_ABGR8UI));
				++ stream_index;
				blend_indices_stream = stream_index;
			}

			merged_vertices.resize(merged_ves.size());
		}

		{
			for (auto const & node : nodes_)
			{
				auto const trans0_mat = node.lod_transforms[0] * global_transform;
				for (auto const mesh_index : node.mesh_indices)
				{
					auto const pos_bb = MathLib::transform_aabb(meshes_[mesh_index].pos_bb, trans0_mat);
					auto const & tc_bb = meshes_[mesh_index].tc_bb;

					float3 const pos_center = pos_bb.Center();
					float3 const pos_extent = pos_bb.HalfSize();
					float3 const tc_center = tc_bb.Center();
					float3 const tc_extent = tc_bb.HalfSize();

					for (uint32_t lod = 0; lod < num_lods; ++ lod)
					{
						float4x4 const trans_mat = node.lod_transforms[lod] * global_transform;
						float4x4 const trans_mat_it = MathLib::transpose(MathLib::inverse(trans_mat));

						auto& mesh_lod = meshes_[mesh_index].lods[lod];

						for (auto const & position : mesh_lod.positions)
						{
							float3 const pos = (MathLib::transform_coord(position, trans_mat) - pos_center) / pos_extent * 0.5f + 0.5f;
							int16_t const s_pos[] =
							{
								static_cast<int16_t>(
									MathLib::clamp<int32_t>(static_cast<int32_t>(pos.x() * 65535 - 32768), -32768, 32767)),
								static_cast<int16_t>(
									MathLib::clamp<int32_t>(static_cast<int32_t>(pos.y() * 65535 - 32768), -32768, 32767)),
								static_cast<int16_t>(
									MathLib::clamp<int32_t>(static_cast<int32_t>(pos.z() * 65535 - 32768), -32768, 32767)),
								32767
							};

							uint8_t const * p = reinterpret_cast<uint8_t const *>(s_pos);
							merged_vertices[position_stream].insert(merged_vertices[position_stream].end(), p, p + sizeof(s_pos));
						}
						if (normal_stream != -1)
						{
							for (auto const & n : mesh_lod.normals)
							{
								float3 const normal = MathLib::normalize(MathLib::transform_normal(n, trans_mat_it)) * 0.5f + 0.5f;
								uint32_t const compact = MathLib::clamp(static_cast<uint32_t>(normal.x() * 255 + 0.5f), 0U, 255U)
									| (MathLib::clamp(static_cast<uint32_t>(normal.y() * 255 + 0.5f), 0U, 255U) << 8)
									| (MathLib::clamp(static_cast<uint32_t>(normal.z() * 255 + 0.5f), 0U, 255U) << 16);

								uint8_t const * p = reinterpret_cast<uint8_t const *>(&compact);
								merged_vertices[normal_stream].insert(merged_vertices[normal_stream].end(), p, p + sizeof(compact));
							}
						}
						if (tangent_quat_stream != -1)
						{
							for (size_t i = 0; i < mesh_lod.tangents.size(); ++ i)
							{
								float3 const tangent = MathLib::normalize(MathLib::transform_normal(mesh_lod.tangents[i], trans_mat));
								float3 const binormal = MathLib::normalize(MathLib::transform_normal(mesh_lod.binormals[i], trans_mat));
								float3 const normal = MathLib::normalize(MathLib::transform_normal(mesh_lod.normals[i], trans_mat_it));

								Quaternion const tangent_quat = MathLib::to_quaternion(tangent, binormal, normal, 8);

								uint32_t const compact = (
									MathLib::clamp(
										static_cast<uint32_t>((tangent_quat.x() * 0.5f + 0.5f) * 255 + 0.5f), 0U, 255U) << 0)
									| (MathLib::clamp(
										static_cast<uint32_t>((tangent_quat.y() * 0.5f + 0.5f) * 255 + 0.5f), 0U, 255U) << 8)
									| (MathLib::clamp(
										static_cast<uint32_t>((tangent_quat.z() * 0.5f + 0.5f) * 255 + 0.5f), 0U, 255U) << 16)
									| (MathLib::clamp(
										static_cast<uint32_t>((tangent_quat.w() * 0.5f + 0.5f) * 255 + 0.5f), 0U, 255U) << 24);

								uint8_t const * p = reinterpret_cast<uint8_t const *>(&compact);
								merged_vertices[tangent_quat_stream].insert(merged_vertices[tangent_quat_stream].end(),
									p, p + sizeof(compact));
							}
						}
						if (diffuse_stream != -1)
						{
							for (auto const & diffuse : mesh_lod.diffuses)
							{
								uint32_t const clr = diffuse.ABGR();

								uint8_t const * p = reinterpret_cast<uint8_t const *>(&clr);
								merged_vertices[tangent_quat_stream].insert(merged_vertices[tangent_quat_stream].end(),
									p, p + sizeof(clr));
							}
						}
						if (specular_stream != -1)
						{
							for (auto const & specular : mesh_lod.speculars)
							{
								uint32_t const clr = specular.ABGR();

								uint8_t const * p = reinterpret_cast<uint8_t const *>(&clr);
								merged_vertices[tangent_quat_stream].insert(merged_vertices[tangent_quat_stream].end(),
									p, p + sizeof(clr));
							}
						}
						if (texcoord_stream != -1)
						{
							for (auto const & tc : mesh_lod.texcoords[0])
							{
								float3 tex_coord = float3(tc.x(), tc.y(), 0.0f);
								tex_coord = (tex_coord - tc_center) / tc_extent * 0.5f + 0.5f;
								int16_t s_tc[2] =
								{
									static_cast<int16_t>(MathLib::clamp<int32_t>(static_cast<int32_t>(tex_coord.x() * 65535 - 32768),
										-32768, 32767)),
									static_cast<int16_t>(MathLib::clamp<int32_t>(static_cast<int32_t>(tex_coord.y() * 65535 - 32768),
										-32768, 32767)),
								};

								uint8_t const * p = reinterpret_cast<uint8_t const *>(s_tc);
								merged_vertices[texcoord_stream].insert(merged_vertices[texcoord_stream].end(), p, p + sizeof(s_tc));
							}
						}

						if (blend_weights_stream != -1)
						{
							BOOST_ASSERT(blend_indices_stream != -1);

							for (auto const & binding : mesh_lod.joint_bindings)
							{
								size_t constexpr MAX_BINDINGS = 4;

								float total_weight = 0;
								size_t const num = std::min(MAX_BINDINGS, binding.size());
								for (size_t wi = 0; wi < num; ++ wi)
								{
									total_weight += binding[wi].second;
								}

								uint8_t joint_ids[MAX_BINDINGS];
								uint8_t weights[MAX_BINDINGS];
								for (size_t wi = 0; wi < num; ++ wi)
								{
									joint_ids[wi] = static_cast<uint8_t>(binding[wi].first);

									float const w = binding[wi].second / total_weight;
									weights[wi] = static_cast<uint8_t>(MathLib::clamp(static_cast<uint32_t>(w * 255 + 0.5f), 0U, 255U));
								}
								for (size_t wi = num; wi < MAX_BINDINGS; ++ wi)
								{
									joint_ids[wi] = 0;
									weights[wi] = 0;
								}

								merged_vertices[blend_weights_stream].insert(merged_vertices[blend_weights_stream].end(),
									weights, weights + sizeof(weights));
								merged_vertices[blend_indices_stream].insert(merged_vertices[blend_indices_stream].end(),
									joint_ids, joint_ids + sizeof(joint_ids));
							}
						}

						mesh_num_vertices.push_back(static_cast<uint32_t>(mesh_lod.positions.size()));
						mesh_base_vertices.push_back(mesh_base_vertices.back() + mesh_num_vertices.back());
					}
				}
			}
		}

		{
			uint32_t max_index = 0;
			for (auto const & mesh : meshes_)
			{
				for (auto const & mesh_lod : mesh.lods)
				{
					for (auto index : mesh_lod.indices)
					{
						max_index = std::max(max_index, index);
					}
				}
			}

			is_index_16_bit = (max_index < 0xFFFF);

			for (auto const & node : nodes_)
			{
				for (auto const mesh_index : node.mesh_indices)
				{
					for (auto const & mesh_lod : meshes_[mesh_index].lods)
					{
						for (auto const index : mesh_lod.indices)
						{
							if (is_index_16_bit)
							{
								uint16_t const i16 = static_cast<uint16_t>(index);

								uint8_t const * p = reinterpret_cast<uint8_t const *>(&i16);
								merged_indices.insert(merged_indices.end(), p, p + sizeof(i16));
							}
							else
							{
								uint32_t const i32 = index;

								uint8_t const * p = reinterpret_cast<uint8_t const *>(&i32);
								merged_indices.insert(merged_indices.end(), p, p + sizeof(i32));
							}
						}

						mesh_num_indices.push_back(static_cast<uint32_t>(mesh_lod.indices.size()));
						mesh_start_indices.push_back(mesh_start_indices.back() + mesh_num_indices.back());
					}
				}
			}
		}

		std::vector<GraphicsBufferPtr> merged_vbs(merged_vertices.size());
		for (size_t i = 0; i < merged_vertices.size(); ++ i)
		{
			auto vb = MakeSharedPtr<SoftwareGraphicsBuffer>(static_cast<uint32_t>(merged_vertices[i].size()), false);
			vb->CreateHWResource(merged_vertices[i].data());

			merged_vbs[i] = vb;
		}
		auto merged_ib = MakeSharedPtr<SoftwareGraphicsBuffer>(static_cast<uint32_t>(merged_indices.size()), false);
		merged_ib->CreateHWResource(merged_indices.data());

		uint32_t mesh_lod_index = 0;
		std::vector<StaticMeshPtr> render_meshes;
		for (auto const & node : nodes_)
		{
			std::wstring wname;
			KlayGE::Convert(wname, node.name);

			auto const trans0_mat = node.lod_transforms[0] * global_transform;
			for (auto const mesh_index : node.mesh_indices)
			{
				StaticMeshPtr render_mesh;
				if (skinned)
				{
					render_mesh = MakeSharedPtr<SkinnedMesh>(render_model_, wname);
				}
				else
				{
					render_mesh = MakeSharedPtr<StaticMesh>(render_model_, wname);
				}
				render_meshes.push_back(render_mesh);

				render_mesh->MaterialID(meshes_[mesh_index].mtl_id);
				render_mesh->PosBound(MathLib::transform_aabb(meshes_[mesh_index].pos_bb, trans0_mat));
				render_mesh->TexcoordBound(meshes_[mesh_index].tc_bb);

				render_mesh->NumLods(num_lods);
				for (uint32_t lod = 0; lod < num_lods; ++ lod, ++ mesh_lod_index)
				{
					for (uint32_t ve_index = 0; ve_index < merged_vertices.size(); ++ ve_index)
					{
						render_mesh->AddVertexStream(lod, merged_vbs[ve_index], merged_ves[ve_index]);
					}
					render_mesh->AddIndexStream(lod, merged_ib, is_index_16_bit ? EF_R16UI : EF_R32UI);

					render_mesh->NumVertices(lod, mesh_num_vertices[mesh_lod_index]);
					render_mesh->NumIndices(lod, mesh_num_indices[mesh_lod_index]);
					render_mesh->StartVertexLocation(lod, mesh_base_vertices[mesh_lod_index]);
					render_mesh->StartIndexLocation(lod, mesh_start_indices[mesh_lod_index]);
				}
			}
		}

		if (skinned)
		{
			auto& skinned_model = *checked_pointer_cast<SkinnedModel>(render_model_);

			for (auto& joint : joints_)
			{
				std::tie(joint.inverse_origin_real, joint.inverse_origin_dual) = MathLib::inverse(joint.bind_real, joint.bind_dual);
				joint.inverse_origin_scale = 1 / joint.bind_scale;
			}
			skinned_model.AssignJoints(joints_.begin(), joints_.end());

			// TODO: Run skinning on CPU to get the bounding box
			uint32_t total_mesh_index = 0;
			for (uint32_t node_index = 0; node_index < nodes_.size(); ++ node_index)
			{
				auto const & node = nodes_[node_index];
				for (size_t mesh_index = 0; mesh_index < node.mesh_indices.size(); ++ mesh_index, ++ total_mesh_index)
				{
					auto& skinned_mesh = *checked_pointer_cast<SkinnedMesh>(render_meshes[total_mesh_index]);

					auto frame_pos_aabbs = MakeSharedPtr<AABBKeyFrameSet>();

					frame_pos_aabbs->frame_id.resize(2);
					frame_pos_aabbs->bb.resize(2);

					frame_pos_aabbs->frame_id[0] = 0;
					frame_pos_aabbs->frame_id[1] = skinned_model.NumFrames() - 1;

					frame_pos_aabbs->bb[0] = skinned_mesh.PosBound();
					frame_pos_aabbs->bb[1] = skinned_mesh.PosBound();

					skinned_mesh.AttachFramePosBounds(frame_pos_aabbs);
				}
			}
		}

		render_model_->AssignSubrenderables(render_meshes.begin(), render_meshes.end());

		if (!in_path)
		{
			ResLoader::Instance().DelPath(in_folder);
		}

		return render_model_;
	}
}

extern "C"
{
	using namespace KlayGE;

	KLAYGE_SYMBOL_EXPORT void ConvertModel(std::string_view input_name, std::string_view metadata_name, std::string_view output_name,
		RenderDeviceCaps const * caps)
	{
		KFL_UNUSED(caps);

		MeshMetadata metadata;
		if (!metadata_name.empty())
		{
			metadata.Load(metadata_name);
		}

		MeshConverter mc;
		auto model = mc.Convert(input_name, metadata);

		std::filesystem::path input_path(input_name.begin(), input_name.end());
		std::filesystem::path output_path(output_name.begin(), output_name.end());
		if (output_path.parent_path() == input_path.parent_path())
		{
			output_path = std::filesystem::path(ResLoader::Instance().Locate(input_name)).parent_path() / output_path.filename();
		}

		auto const outptu_ext = output_path.extension().string();
		if (outptu_ext == ".model_bin")
		{
			SaveModel(model, output_path.string());
		}
		else
		{
			mc.SaveByAssimp(output_path.string());
		}
	}
}
