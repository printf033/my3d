#pragma once

#include "logger.hpp"
#include "node.hpp"
#include "vertex.hpp"
#include "converter.hpp"
#include <unordered_map>
#include <shared_mutex>
#include <fstream>
#include <vector>
#include <atomic>
#include <memory>
#include <string>
#include <cassert>
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <stb/stb_image.h>
#include <filament/Engine.h>
#include <filament/Renderer.h>
#include <filament/TextureSampler.h>
#include <filament/Material.h>
#include <filament/View.h>
#include <filament/Scene.h>
#include <utils/Entity.h>
#include <utils/EntityManager.h>
#include <filament/TransformManager.h>
#include <filament/RenderableManager.h>
#include <filament/VertexBuffer.h>
#include <filament/IndexBuffer.h>
#include <filament/Texture.h>
#include <math/mat3.h>
#include <math/vec3.h>
#include <math/quat.h>

class Engine
{
    filament::Engine *engineGPU_ = nullptr;
    filament::Renderer *rendererGPU_ = nullptr;
    filament::VertexBuffer *verticesGPU_ = nullptr;
    filament::IndexBuffer *indicesGPU_ = nullptr;
    std::unordered_map<std::string, filament::Texture *> texturesGPU_;
    std::unordered_map<std::string, filament::Material *> shadersGPU_;
    // std::unordered_map<std::string, filament:: *> lightsGPU_;
    std::unordered_map<std::string, filament::View *> viewsGPU_;
    std::unordered_map<std::string, filament::Camera *> camerasGPU_;
    std::unordered_map<std::string, utils::Entity> entitiesGPU_;
    std::unordered_map<std::string, filament::Scene *> scenesGPU_;

    std::vector<Vertex> verticesCPU_;
    std::vector<uint32_t> indicesCPU_;
    std::unordered_map<std::string, Node> modelsCPU_;

public:
    Engine() noexcept { engineGPU_ = filament::Engine::create(); }
    ~Engine() noexcept = default;
    Engine(const Engine &) = delete;
    Engine &operator=(const Engine &) = delete;
    Engine(Engine &&) noexcept = delete;
    Engine &operator=(Engine &&) noexcept = delete;
    void loadModel(const std::string &file, const std::string &filamat)
    {
        if (modelsCPU_.find(file) != modelsCPU_.end())
            return;
        Assimp::Importer importer;
        auto scene = importer.ReadFile(file,
                                       aiProcess_Triangulate |
                                           aiProcess_CalcTangentSpace |
                                           aiProcess_GenSmoothNormals |
                                           aiProcess_GenBoundingBoxes |
                                           aiProcess_JoinIdenticalVertices |
                                           aiProcess_ImproveCacheLocality |
                                           aiProcess_FindInvalidData |
                                           aiProcess_FlipUVs);
        if (scene && scene->mRootNode)
        {
            Node tmp = importNode(scene->mRootNode, scene->mMeshes, scene->mMaterials, filamat);
            modelsCPU_.emplace(file, std::move(tmp));
            if (scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE)
                LOG_ERROR(importer.GetErrorString());
        }
    }
    inline void asyncVerticesIndices2GPU() noexcept
    {
        verticesGPU_ = filament::VertexBuffer::Builder()
                           .vertexCount(verticesCPU_.size())
                           .bufferCount(1)
                           .attribute(filament::VertexAttribute::POSITION, 0, filament::VertexBuffer::AttributeType::FLOAT3, 0, sizeof(Vertex))
                           .attribute(filament::VertexAttribute::TANGENTS, 0, filament::VertexBuffer::AttributeType::FLOAT4, offsetof(Vertex, tbn), sizeof(Vertex))
                           .attribute(filament::VertexAttribute::UV0, 0, filament::VertexBuffer::AttributeType::FLOAT2, offsetof(Vertex, uv), sizeof(Vertex))
                           .build(*engineGPU_);
        verticesGPU_->setBufferAt(*engineGPU_, 0,
                                  filament::VertexBuffer::BufferDescriptor(
                                      verticesCPU_.data(),
                                      verticesCPU_.size() * sizeof(Vertex)));
        indicesGPU_ = filament::IndexBuffer::Builder()
                          .indexCount(indicesCPU_.size())
                          .bufferType(filament::IndexBuffer::IndexType::UINT)
                          .build(*engineGPU_);
        indicesGPU_->setBuffer(*engineGPU_,
                               filament::IndexBuffer::BufferDescriptor(
                                   indicesCPU_.data(),
                                   indicesCPU_.size() * sizeof(uint32_t)));
    }
    inline filament::Engine *getEngine() noexcept
    {
        return engineGPU_;
    }
    inline filament::Renderer *getRenderer() noexcept
    {
        if (rendererGPU_)
            return rendererGPU_;
        return rendererGPU_ = engineGPU_->createRenderer();
    }
    inline filament::View *getView(const std::string &name)
    {
        if (viewsGPU_.find(name) != viewsGPU_.end())
            return viewsGPU_[name];
        viewsGPU_.emplace(name, engineGPU_->createView());
        return viewsGPU_[name];
    }
    inline filament::Camera *getCamera(const std::string &name)
    {
        if (camerasGPU_.find(name) != camerasGPU_.end())
            return camerasGPU_[name];
        camerasGPU_.emplace(name, engineGPU_->createCamera(getEntity(name)));
        return camerasGPU_[name];
    }
    inline filament::Scene *getScene(const std::string &name)
    {
        if (scenesGPU_.find(name) != scenesGPU_.end())
            return scenesGPU_[name];
        scenesGPU_.emplace(name, engineGPU_->createScene());
        return scenesGPU_[name];
    }
    inline utils::Entity getEntity(const std::string &name)
    {
        if (entitiesGPU_.find(name) != entitiesGPU_.end())
            return entitiesGPU_[name];
        entitiesGPU_.emplace(name, utils::EntityManager::get().create());
        return entitiesGPU_[name];
    }
    inline void addModel2Scene(const std::string &sceneName, const std::string &modelName, const std::string &loadedFile) noexcept
    {
        auto scene = getScene(sceneName);
        auto &model = modelsCPU_[loadedFile];
        auto dst = getEntity(modelName);
        scene->addEntity(dst);
        syncNode2GPU(dst, model);
        for (auto &[name, child] : model.children)
        {
            auto entity = getEntity(name);
            scene->addEntity(entity);
            syncNode2GPU(entity, child);
            syncParent2GPU(entity, dst);
        }
        syncTransform2GPU(dst, model.transform);
    }
    inline void syncNode2GPU(utils::Entity dst, Node &node) noexcept
    {
        filament::RenderableManager::Builder builder(node.meshes.size());
        builder.boundingBox(node.bound);
        size_t idx = 0;
        for (auto &[_, primitive] : node.meshes)
        {
            builder.geometry(idx,
                             filament::RenderableManager::PrimitiveType::TRIANGLES,
                             verticesGPU_,
                             indicesGPU_,
                             primitive.indexOffset,
                             primitive.indexCount)
                .material(idx,
                          primitive.material.material);
            ++idx;
        }
        builder.build(*engineGPU_, dst);
        syncTransform2GPU(dst, node.transform);
    }
    inline void syncTransform2GPU(utils::Entity dst, filament::math::mat4f &transform) noexcept
    {
        auto &manager = engineGPU_->getTransformManager();
        manager.setTransform(manager.getInstance(dst), transform);
    }
    inline void syncParent2GPU(utils::Entity dst, utils::Entity parent) noexcept
    {
        auto &manager = engineGPU_->getTransformManager();
        manager.setParent(manager.getInstance(dst), manager.getInstance(parent));
    }

private:
    Node importNode(aiNode *node, aiMesh *meshes[], aiMaterial *materials[], const std::string &filamat)
    {
        assert(node);
        assert(meshes);
        assert(materials);
        Node tmp;
        tmp.transform = Converter::assimp2filament(node->mTransformation);
        tmp.meshes.reserve(node->mNumMeshes);
        filament::math::float3 nodeMin;
        filament::math::float3 nodeMax;
        for (unsigned int i = 0; i < node->mNumMeshes; ++i)
        {
            aiMesh *mesh = meshes[node->mMeshes[i]];
            std::string name = mesh->mName.C_Str();
            tmp.meshes.emplace(name, importMesh(mesh, materials[mesh->mMaterialIndex], filamat));
            filament::math::float3 meshMin = tmp.meshes[name].bound.getMin();
            filament::math::float3 meshMax = tmp.meshes[name].bound.getMax();
            if (i != 0)
            {
                nodeMin.x = std::min(nodeMin.x, meshMin.x);
                nodeMin.y = std::min(nodeMin.y, meshMin.y);
                nodeMin.z = std::min(nodeMin.z, meshMin.z);
                nodeMax.x = std::max(nodeMax.x, meshMax.x);
                nodeMax.y = std::max(nodeMax.y, meshMax.y);
                nodeMax.z = std::max(nodeMax.z, meshMax.z);
            }
            else
            {
                nodeMin = meshMin;
                nodeMax = meshMax;
            }
        }
        tmp.children.reserve(node->mNumChildren);
        for (unsigned int i = 0; i < node->mNumChildren; ++i)
        {
            aiNode *child = node->mChildren[i];
            std::string name = child->mName.C_Str();
            tmp.children.emplace(name, importNode(child, meshes, materials, filamat));
            filament::math::float3 childMin = tmp.children[name].bound.getMin();
            filament::math::float3 childMax = tmp.children[name].bound.getMax();
            if (i != 0)
            {
                nodeMin.x = std::min(nodeMin.x, childMin.x);
                nodeMin.y = std::min(nodeMin.y, childMin.y);
                nodeMin.z = std::min(nodeMin.z, childMin.z);
                nodeMax.x = std::max(nodeMax.x, childMax.x);
                nodeMax.y = std::max(nodeMax.y, childMax.y);
                nodeMax.z = std::max(nodeMax.z, childMax.z);
            }
            else
            {
                nodeMin = childMin;
                nodeMax = childMax;
            }
        }
        tmp.bound.set(nodeMin, nodeMax);
        return tmp;
    }
    Mesh importMesh(aiMesh *mesh, aiMaterial *material, const std::string &filamat)
    {
        assert(mesh);
        assert(material);
        Mesh tmp;
        uint32_t base = static_cast<uint32_t>(verticesCPU_.size());
        tmp.indexOffset = static_cast<uint32_t>(indicesCPU_.size());
        tmp.indexCount = mesh->mNumFaces * 3U;
        verticesCPU_.resize(verticesCPU_.size() + mesh->mNumVertices);
        for (unsigned int i = 0; i < mesh->mNumVertices; ++i)
        {
            verticesCPU_[base + i] = importVertex(mesh, i);
        }
        indicesCPU_.reserve(indicesCPU_.size() + mesh->mNumFaces * 3);
        for (unsigned int i = 0; i < mesh->mNumFaces; ++i)
        {
            for (unsigned int j = 0; j < 3U; ++j)
            {
                uint32_t index = static_cast<uint32_t>(mesh->mFaces[i].mIndices[j]);
                indicesCPU_.push_back(base + index);
            }
        }
        tmp.bound.set(Converter::assimp2filament(mesh->mAABB.mMin), Converter::assimp2filament(mesh->mAABB.mMax));
        tmp.material = importMaterial(material, filamat);
        return tmp;
    }
    Vertex importVertex(aiMesh *mesh, unsigned int i)
    {
        assert(mesh);
        Vertex tmp;
        tmp.xyz = {mesh->mVertices[i].x, mesh->mVertices[i].y, mesh->mVertices[i].z};
        if (mesh->HasNormals())
        {
            filament::math::float3 n = {mesh->mNormals[i].x, mesh->mNormals[i].y, mesh->mNormals[i].z};
            if (mesh->HasTangentsAndBitangents())
            {
                filament::math::float3 t = {mesh->mTangents[i].x, mesh->mTangents[i].y, mesh->mTangents[i].z};
                filament::math::float3 b = {mesh->mBitangents[i].x, mesh->mBitangents[i].y, mesh->mBitangents[i].z};
                tmp.tbn = filament::math::mat3f::packTangentFrame({t, b, n});
            }
            else
            {
                filament::math::float3 up = (std::abs(n.z) < 0.999f) ? filament::math::float3{0.0f, 0.0f, 1.0f} : filament::math::float3{1.0f, 0.0f, 0.0f};
                filament::math::float3 t = normalize(cross(up, n));
                filament::math::float3 b = cross(n, t);
                tmp.tbn = filament::math::mat3f::packTangentFrame({t, b, n});
            }
        }
        else
        {
            tmp.tbn = {0.0f, 0.0f, 0.0f, 1.0f};
        }
        if (mesh->HasTextureCoords(0))
        {
            tmp.uv = {mesh->mTextureCoords[0][i].x, mesh->mTextureCoords[0][i].y};
        }
        else
        {
            tmp.uv = {0.0f, 0.0f};
        }
        return tmp;
    }
    Material importMaterial(aiMaterial *material, const std::string &filamat)
    {
        assert(material);
        Material tmp;
        tmp.material = loadShader(filamat)->createInstance();
        material->Get(AI_MATKEY_COLOR_DIFFUSE, tmp.albedo);
        material->Get(AI_MATKEY_ROUGHNESS_FACTOR, tmp.roughness);
        material->Get(AI_MATKEY_METALLIC_FACTOR, tmp.metallic);
        tmp.material->setParameter("albedo", tmp.albedo);
        tmp.material->setParameter("roughness", tmp.roughness);
        tmp.material->setParameter("metallic", tmp.metallic);
        filament::TextureSampler sampler(filament::TextureSampler::MinFilter::LINEAR_MIPMAP_LINEAR,
                                         filament::TextureSampler::MagFilter::LINEAR,
                                         filament::TextureSampler::WrapMode::CLAMP_TO_EDGE);
        aiString path;
        if (material->GetTextureCount(aiTextureType_DIFFUSE) > 0)
        {
            if (material->GetTexture(aiTextureType_DIFFUSE, 0, &path) == AI_SUCCESS)
            {
                tmp.material->setParameter("diffuse", loadMaterialAsyncTextures2GPU(path.C_Str()), sampler);
            }
        }
        if (material->GetTextureCount(aiTextureType_NORMALS) > 0)
        {
            if (material->GetTexture(aiTextureType_NORMALS, 0, &path) == AI_SUCCESS)
            {
                tmp.material->setParameter("normal", loadMaterialAsyncTextures2GPU(path.C_Str()), sampler);
            }
        }
        if (material->GetTextureCount(aiTextureType_AMBIENT_OCCLUSION) > 0)
        {
            if (material->GetTexture(aiTextureType_AMBIENT_OCCLUSION, 0, &path) == AI_SUCCESS)
            {
                tmp.material->setParameter("occlusion", loadMaterialAsyncTextures2GPU(path.C_Str()), sampler);
            }
        }
        if (material->GetTextureCount(aiTextureType_SHININESS) > 0)
        {
            if (material->GetTexture(aiTextureType_SHININESS, 0, &path) == AI_SUCCESS)
            {
                tmp.material->setParameter("roughness", loadMaterialAsyncTextures2GPU(path.C_Str()), sampler);
            }
        }
        if (material->GetTextureCount(aiTextureType_METALNESS) > 0)
        {
            if (material->GetTexture(aiTextureType_METALNESS, 0, &path) == AI_SUCCESS)
            {
                tmp.material->setParameter("metallic", loadMaterialAsyncTextures2GPU(path.C_Str()), sampler);
            }
        }
        if (material->GetTextureCount(aiTextureType_EMISSIVE) > 0)
        {
            if (material->GetTexture(aiTextureType_EMISSIVE, 0, &path) == AI_SUCCESS)
            {
                tmp.material->setParameter("emissive", loadMaterialAsyncTextures2GPU(path.C_Str()), sampler);
            }
        }
        return tmp;
    }
    filament::Material *loadShader(const std::string &filamat)
    {
        if (shadersGPU_.find(filamat) != shadersGPU_.end())
            return shadersGPU_[filamat];
        std::ifstream ifs(filamat, std::ios::binary | std::ios::ate);
        if (!ifs)
        {
            LOG_ERROR("cannot open {}", filamat);
            return nullptr;
        }
        std::streamsize size = ifs.tellg();
        ifs.seekg(0, std::ios::beg);
        std::vector<char> buffer(size);
        if (!ifs.read(buffer.data(), size))
        {
            LOG_ERROR("cannot read {}", filamat);
            return nullptr;
        }
        filament::Material *ptr = filament::Material::Builder()
                                      .package(buffer.data(), buffer.size())
                                      .build(*engineGPU_);
        shadersGPU_.emplace(filamat, ptr);
        return ptr;
    }
    filament::Texture *loadMaterialAsyncTextures2GPU(const std::string &file)
    {
        if (texturesGPU_.find(file) != texturesGPU_.end())
            return texturesGPU_[file];
        int width, height, channels;
        stbi_uc *image = stbi_load(file.c_str(), &width, &height, &channels, 4);
        if (!image)
            return nullptr;
        uint32_t levels = static_cast<uint32_t>(std::floor(std::log2(std::max(width, height)))) + 1;
        filament::Texture *ptr = filament::Texture::Builder()
                                     .width(uint32_t(width))
                                     .height(uint32_t(height))
                                     .levels(levels)
                                     .format(filament::Texture::InternalFormat::RGBA8)
                                     .sampler(filament::Texture::Sampler::SAMPLER_2D)
                                     .build(*engineGPU_);
        ptr->setImage(*engineGPU_, 0,
                      filament::Texture::PixelBufferDescriptor(
                          image,
                          size_t(width * height * 4),
                          filament::Texture::Format::RGBA,
                          filament::Texture::Type::UBYTE,
                          [](void *mem, size_t, void *)
                          { stbi_image_free(mem); }));
        texturesGPU_.emplace(file, ptr);
        ptr->generateMipmaps(*engineGPU_);
        return ptr;
    }
};
