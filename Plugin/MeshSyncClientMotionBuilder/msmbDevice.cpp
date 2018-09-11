#include "pch.h"
#include "msmbDevice.h"
#include "msmbUtils.h"


FBDeviceImplementation(msmbDevice);
FBRegisterDevice("msmbDevice", msmbDevice, "UnityMeshSync", "UnityMeshSync for MotionBuilder", FB_DEFAULT_SDK_ICON);

bool msmbDevice::FBCreate()
{
    FBSystem::TheOne().Scene->OnChange.Add(this, (FBCallback)&msmbDevice::onSceneChange);
    FBSystem::TheOne().OnConnectionDataNotify.Add(this, (FBCallback)&msmbDevice::onDataUpdate);
    FBEvaluateManager::TheOne().OnSynchronizationEvent.Add(this, (FBCallback)&msmbDevice::onSynchronization);
    return true;
}

void msmbDevice::FBDestroy()
{
    FBSystem::TheOne().Scene->OnChange.Remove(this, (FBCallback)&msmbDevice::onSceneChange);
    FBSystem::TheOne().OnConnectionDataNotify.Remove(this, (FBCallback)&msmbDevice::onDataUpdate);
    FBEvaluateManager::TheOne().OnSynchronizationEvent.Remove(this, (FBCallback)&msmbDevice::onSynchronization);
}

bool msmbDevice::DeviceOperation(kDeviceOperations pOperation)
{
    return false;
}

void msmbDevice::DeviceTransportNotify(kTransportMode pMode, FBTime pTime, FBTime pSystem)
{
    if (auto_sync)
        m_dirty = true;
}

void msmbDevice::onSceneChange(HIRegister pCaller, HKEventBase pEvent)
{
    FBEventSceneChange SceneChangeEvent = pEvent;
    FBSceneChangeType type = SceneChangeEvent.Type;
    switch (type)
    {
    case kFBSceneChangeDestroy:
    case kFBSceneChangeAttach:
    case kFBSceneChangeDetach:
    case kFBSceneChangeAddChild:
    case kFBSceneChangeRemoveChild:
    case kFBSceneChangeRenamed:
    case kFBSceneChangeRenamedPrefix:
    case kFBSceneChangeRenamedUnique:
    case kFBSceneChangeRenamedUniquePrefix:
    case kFBSceneChangeLoadEnd:
    case kFBSceneChangeClearEnd:
    case kFBSceneChangeTransactionEnd:
    case kFBSceneChangeMergeTransactionEnd:
    case kFBSceneChangeChangeName:
    case kFBSceneChangeChangedName:
        if (type == kFBSceneChangeLoadEnd ||
            type == kFBSceneChangeAddChild)
        {
            m_dirty_meshes = m_dirty_textures = true;
        }
        if (auto_sync)
            m_dirty = true;
        break;

    default:
        break;
    }
}

void msmbDevice::onDataUpdate(HIRegister pCaller, HKEventBase pEvent)
{
    //FBEventConnectionDataNotify	lEvent(pEvent);
    if (auto_sync)
        m_dirty = true;
}

void msmbDevice::onSynchronization(HIRegister pCaller, HKEventBase pEvent)
{
    FBEventEvalGlobalCallback lFBEvent(pEvent);
    FBGlobalEvalCallbackTiming timing = lFBEvent.GetTiming();
    if (timing == kFBGlobalEvalCallbackSyn) {
        if (auto_sync)
            update();
    }
}

void msmbDevice::update()
{
    if (!m_dirty && !m_pending)
        return;
    sendScene(false);
}

bool msmbDevice::isSending() const
{
    if (m_future_send.valid()) {
        return m_future_send.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout;
    }
    return false;
}

void msmbDevice::waitAsyncSend()
{
    if (m_future_send.valid()) {
        m_future_send.wait_for(std::chrono::milliseconds(timeout_ms));
    }
}

void msmbDevice::kickAsyncSend()
{
    // process parallel extract tasks
    if (!m_extract_tasks.empty()) {
        mu::parallel_for_each(m_extract_tasks.begin(), m_extract_tasks.end(), [](ExtractTasks::value_type& task) {
            task();
        });
        m_extract_tasks.clear();
    }


    // begin async send
    m_future_send = std::async(std::launch::async, [this]() {
        ms::Client client(client_settings);

        ms::SceneSettings scene_settings;
        scene_settings.handedness = ms::Handedness::Right;
        scene_settings.scale_factor = scale_factor;

        // notify scene begin
        {
            ms::FenceMessage fence;
            fence.type = ms::FenceMessage::FenceType::SceneBegin;
            client.send(fence);
        }

        // send delete message
        size_t num_deleted = m_deleted.size();
        if (num_deleted) {
            ms::DeleteMessage del;
            del.targets.resize(num_deleted);
            for (uint32_t i = 0; i < num_deleted; ++i)
                del.targets[i].path = m_deleted[i];

            client.send(del);
            m_deleted.clear();
        }

        // send scene data
        {
            ms::SetMessage set;
            set.scene.settings = scene_settings;
            set.scene.objects = m_objects;
            set.scene.textures = m_textures;
            set.scene.materials = m_materials;
            client.send(set);

            m_objects.clear();
            m_textures.clear();
            m_materials.clear();
        }

        // send meshes one by one to Unity can respond quickly
        for (auto& mesh : m_meshes) {
            ms::SetMessage set;
            set.scene.settings = scene_settings;
            set.scene.objects = { mesh };
            client.send(set);
        };
        m_meshes.clear();

        // send animations and constraints
        if (!m_animations.empty() || !m_constraints.empty()) {
            ms::SetMessage set;
            set.scene.settings = scene_settings;
            set.scene.animations = m_animations;
            set.scene.constraints = m_constraints;
            client.send(set);

            m_animations.clear();
            m_constraints.clear();
        }

        // notify scene end
        {
            ms::FenceMessage fence;
            fence.type = ms::FenceMessage::FenceType::SceneEnd;
            client.send(fence);
        }
    });
}


bool msmbDevice::sendScene(bool force_all)
{
    if (force_all)
        m_dirty_meshes = m_dirty_textures = true;

    m_dirty = m_pending = false;
    if (isSending()) {
        m_pending = true;
        return false;
    }

    if (sync_meshes)
        exportMaterials();

    // export nodes
    int num_exported = 0;
    EnumerateAllNodes([this, &num_exported](FBModel* node) {
        if (exportObject(node, false))
            ++num_exported;
    });

    // check deleted objects
    for (auto it = m_node_records.begin(); it != m_node_records.end(); /**/) {
        if (!it->second.exist) {
            m_deleted.push_back(it->second.path);
            m_node_records.erase(it++);
        }
        else {
            ++it;
        }
    }

    // clear state
    for (auto& kvp : m_node_records) {
        kvp.second.dst = nullptr;
        kvp.second.exist = false;
    }
    m_dirty_meshes = m_dirty_textures = false;

    // send
    if (num_exported || !m_deleted.empty()) {
        kickAsyncSend();
        return true;
    }
    else {
        return false;
    }
}

bool msmbDevice::exportObject(FBModel* src, bool force)
{
    if (!src)
        return false;

    auto& rec = m_node_records[src];
    if (rec.dst) {
        return false;
    }
    else if (rec.name.empty()) {
        rec.name = GetName(src);
        rec.path = GetPath(src);
        rec.index = ++m_node_index_seed;
    }
    else if (rec.name != GetName(src)) {
        m_deleted.push_back(rec.path);
        rec.name = GetName(src);
        rec.path = GetPath(src);
    }
    rec.exist = true;

    if (IsCamera(src)) { // camera
        if (sync_cameras) {
            exportObject(src->Parent, true);
            auto obj = ms::Camera::create();
            rec.dst = obj.get();
            m_objects.push_back(obj);
            extractCamera(*obj, static_cast<FBCamera*>(src));
        }
    }
    else if (IsLight(src)) { // light
        if (sync_lights) {
            exportObject(src->Parent, true);
            auto obj = ms::Light::create();
            rec.dst = obj.get();
            m_objects.push_back(obj);
            extractLight(*obj, static_cast<FBLight*>(src));
        }
    }
    else if (IsMesh(src)) { // mesh
        if (sync_meshes && m_dirty_meshes) {
            exportObject(src->Parent, true);
            auto obj = ms::Mesh::create();
            rec.dst = obj.get();
            m_meshes.push_back(obj);
            extractMesh(*obj, src);
        }
    }
    else if (IsBone(src) || force) {
        exportObject(src->Parent, true);
        auto obj = ms::Transform::create();
        rec.dst = obj.get();
        m_objects.push_back(obj);
        extractTransform(*obj, src);
    }

    return rec.dst != nullptr;
}


static void ExtractTransformData(FBModel* src, mu::float3& pos, mu::quatf& rot, mu::float3& scale, bool& vis)
{
    FBMatrix tmp;
    src->GetMatrix(tmp, kModelTransformation, true, nullptr);
    auto trs = to_float4x4(tmp);

    if (src->Parent) {
        src->Parent->GetMatrix(tmp, kModelTransformation, true, nullptr);
        trs *= mu::invert(to_float4x4(tmp));
    }

    pos = extract_position(trs);
    rot = extract_rotation(trs);
    scale = extract_scale(trs);
    vis = src->Visibility;
}

static void ExtractCameraData(FBCamera* src, bool& ortho, float& near_plane, float& far_plane, float& fov,
    float& horizontal_aperture, float& vertical_aperture, float& focal_length, float& focus_distance)
{
    ortho = src->Type == kFBCameraTypeOrthogonal;
    near_plane = (float)src->NearPlaneDistance;
    far_plane = (float)src->FarPlaneDistance;
    fov = (float)src->FieldOfViewY;
}

static void ExtractLightData(FBLight* src, ms::Light::LightType& type, mu::float4& color, float& intensity, float& spot_angle)
{
    FBLightType light_type = src->LightType;
    if (light_type == kFBLightTypePoint) {
        type = ms::Light::LightType::Point;
    }
    else if (light_type == kFBLightTypeInfinite) {
        type = ms::Light::LightType::Directional;
    }
    else if (light_type == kFBLightTypeSpot) {
        type = ms::Light::LightType::Spot;
        spot_angle = (float)src->OuterAngle;
    }
    else if (light_type == kFBLightTypeArea) {
        type = ms::Light::LightType::Area;
    }

    color = to_float4(src->DiffuseColor);
    intensity = (float)src->Intensity * 0.01f;
}


void msmbDevice::extractTransform(ms::Transform& dst, FBModel* src)
{
    dst.path = GetPath(src);
    ExtractTransformData(src, dst.position, dst.rotation, dst.scale, dst.visible);
}

void msmbDevice::extractCamera(ms::Camera& dst, FBCamera* src)
{
    extractTransform(dst, src);
    dst.rotation *= mu::rotateY(90.0f * mu::Deg2Rad);

    ExtractCameraData(src, dst.is_ortho, dst.near_plane, dst.far_plane, dst.fov,
        dst.horizontal_aperture, dst.vertical_aperture, dst.focal_length, dst.focus_distance);
}

void msmbDevice::extractLight(ms::Light& dst, FBLight* src)
{
    extractTransform(dst, src);
    dst.rotation *= mu::rotateX(90.0f * mu::Deg2Rad);

    ExtractLightData(src, dst.light_type, dst.color, dst.intensity, dst.spot_angle);
}

void msmbDevice::extractMesh(ms::Mesh& dst, FBModel* src)
{
    extractTransform(dst, src);
    doExtractMesh(dst, src);
}

void msmbDevice::doExtractMesh(ms::Mesh & dst, FBModel * src)
{
    FBModelVertexData *vd = src->ModelVertexData;
    int num_vertices = vd->GetVertexCount();
    auto points = (const FBVertex*)vd->GetVertexArray(kFBGeometryArrayID_Point, false);
    auto normals = (const FBNormal*)vd->GetVertexArray(kFBGeometryArrayID_Normal, false);
    auto uvs = (const FBUV*)vd->GetUVSetArray();

    if (!points)
        return;

    // get vertex arrays
    {
        dst.points.resize_discard(num_vertices);
        for (int vi = 0; vi < num_vertices; ++vi)
            dst.points[vi] = to_float3(points[vi]);
    }

    if (normals) {
        dst.normals.resize_discard(num_vertices);
        for (int vi = 0; vi < num_vertices; ++vi)
            dst.normals[vi] = to_float3(normals[vi]);
    }
    else {
        dst.refine_settings.flags.gen_normals = 1;
    }

    if (uvs) {
        dst.uv0.resize_discard(num_vertices);
        for (int vi = 0; vi < num_vertices; ++vi)
            dst.uv0[vi] = to_float2(uvs[vi]);
        dst.refine_settings.flags.gen_tangents = 1;
    }

    // process skin cluster
    if (FBCluster *cluster = src->Cluster) {
        //DbgPrintCluster(src);

        int num_links = cluster->LinkGetCount();
        for (int li = 0; li < num_links; ++li) {
            ClusterScope scope(cluster, li);

            int n = cluster->VertexGetCount();
            if (n == 0)
                continue;;

            auto bd = ms::BoneData::create();
            dst.bones.push_back(bd);

            auto bone = cluster->LinkGetModel(li);
            bd->path = GetPath(bone);

            // root bone
            if (li == 0) {
                auto root = bone;
                while (IsBone(root->Parent))
                    root = root->Parent;
                dst.root_bone = GetPath(root);
            }

            // bindpose
            {
                // should consider rotation order?
                //FBModelRotationOrder order = bone->RotationOrder;

                FBVector3d t,r,s;
                cluster->VertexGetTransform(t, r, s);
                mu::quatf q = mu::invert(mu::rotateXYZ(to_float3(r) * mu::Deg2Rad));
                bd->bindpose = mu::transform(to_float3(t), q, to_float3(s));
            }

            // weights
            bd->weights.resize_zeroclear(num_vertices);
            for (int vi = 0; vi < n; ++vi) {
                int i = cluster->VertexGetNumber(vi);
                float w = (float)cluster->VertexGetWeight(vi);
                bd->weights[i] = w;
            }
        }
    }

    // blendshapes
    if (FBGeometry *geom = src->Geometry) {
        int num_shapes = geom->ShapeGetCount();
        if (num_shapes) {
            RawVector<mu::float3> tmp_points, tmp_normals;
            for (int si = 0; si < num_shapes; ++si) {
                auto bsd = ms::BlendShapeData::create();
                dst.blendshapes.push_back(bsd);
                bsd->name = geom->ShapeGetName(si);

                auto bsfd = ms::BlendShapeFrameData::create();
                bsd->frames.push_back(bsfd);

                tmp_points = dst.points;
                tmp_normals = dst.normals;

                int num_points = geom->ShapeGetDiffPointCount(si);
                if (!tmp_normals.empty()) {
                    for (int pi = 0; pi < num_points; ++pi) {
                        int vi;
                        FBVertex p, n;
                        if (geom->ShapeGetDiffPoint(si, pi, vi, p, n)) {
                            tmp_points[vi] += to_float3(p);
                            tmp_normals[vi] += to_float3(n);
                        }
                    }
                    bsfd->points = std::move(tmp_points);
                    bsfd->normals = std::move(tmp_normals);
                }
                else {
                    for (int pi = 0; pi < num_points; ++pi) {
                        int vi;
                        FBVertex p;
                        if (geom->ShapeGetDiffPoint(si, pi, vi, p)) {
                            tmp_points[vi] += to_float3(p);
                        }
                    }
                    auto bsfd = ms::BlendShapeFrameData::create();
                    bsfd->points = std::move(tmp_points);
                }
            }
        }
    }

    // enumerate subpatches ("submeshes" in Unity's term) and generate indices
    int num_subpatches = vd->GetSubPatchCount();
    auto indices = (const int*)vd->GetIndexArray();
    for (int spi = 0; spi < num_subpatches; ++spi) {
        int offset = vd->GetSubPatchIndexOffset(spi);
        int count = vd->GetSubPatchIndexSize(spi);
        int mid = m_material_records[vd->GetSubPatchMaterial(spi)].id;
        auto idx_begin = indices + offset;
        auto idx_end = idx_begin + count;

        int ngon = 1;
        switch (vd->GetSubPatchPrimitiveType(spi)) {
        case kFBGeometry_POINTS:    ngon = 1; break;
        case kFBGeometry_LINES:     ngon = 2; break;
        case kFBGeometry_TRIANGLES: ngon = 3; break;
        case kFBGeometry_QUADS:     ngon = 4; break;
        // todo: support other topologies (triangle strip, etc)
        default: continue;
        }
        int prim_count = count / ngon;

        dst.indices.insert(dst.indices.end(), idx_begin, idx_end);
        dst.counts.resize(dst.counts.size() + prim_count, ngon);
        dst.material_ids.resize(dst.material_ids.size() + prim_count, mid);
    }

    dst.refine_settings.flags.swap_faces = 1;
    dst.setupFlags();
}


bool msmbDevice::exportMaterials()
{
    int num_exported = 0;

    auto& materials = FBSystem::TheOne().Scene->Materials;
    const int num_materials = materials.GetCount();
    for (int mi = 0; mi < num_materials; ++mi) {
        if (exportMaterial(materials[mi]))
            ++num_exported;
    }

    for (auto& kvp : m_texture_records)
        kvp.second.dst = nullptr;
    for (auto& kvp : m_material_records)
        kvp.second.dst = nullptr;

    return num_exported > 0;
}

int msmbDevice::exportTexture(FBTexture* src, FBMaterialTextureType type)
{
    if (!src)
        return -1;

    FBVideoClip* video = FBCast<FBVideoClip>(src->Video);
    if (!video)
        return -1;

    auto& rec = m_texture_records[src];
    if (rec.dst)
        return rec.id; // already exported

    auto dst = ms::Texture::create();
    m_textures.push_back(dst);
    rec.dst = dst.get();
    if (rec.id == -1)
        rec.id = (int)m_texture_records.size() - 1;

    dst->id = rec.id;
    if (type == kFBMaterialTextureNormalMap)
        dst->type = ms::TextureType::NormalMap;

    RawVector<char> data;
    if (ms::FileToByteArray(video->Filename, data)) {
        // send raw file contents

        dst->name = mu::GetFilename(video->Filename);
        dst->format = ms::TextureFormat::RawFile;
        dst->data = std::move(data);
    }
    else {
        // send texture data in FBVideoClip

        dst->name = mu::GetFilename_NoExtension(video->Filename);
        dst->width = video->Width;
        dst->height = video->Height;

        int num_pixels = dst->width * dst->height;
        auto image = (const char*)video->GetImage();

        switch (video->Format) {
        case kFBVideoFormat_RGBA_32:
            dst->format = ms::TextureFormat::RGBAu8;
            data.assign(image, image + (num_pixels * 4));
            break;
        case kFBVideoFormat_ABGR_32:
            dst->format = ms::TextureFormat::RGBAu8;
            data.resize_discard(num_pixels * 4);
            mu::ABGR2RGBA((mu::unorm8x4*)data.data(), (mu::unorm8x4*)image, num_pixels);
            break;
        case kFBVideoFormat_ARGB_32:
            dst->format = ms::TextureFormat::RGBAu8;
            data.resize_discard(num_pixels * 4);
            mu::ARGB2RGBA((mu::unorm8x4*)data.data(), (mu::unorm8x4*)image, num_pixels);
            break;
        case kFBVideoFormat_BGRA_32:
            dst->format = ms::TextureFormat::RGBAu8;
            data.resize_discard(num_pixels * 4);
            mu::BGRA2RGBA((mu::unorm8x4*)data.data(), (mu::unorm8x4*)image, num_pixels);
            break;
        case kFBVideoFormat_RGB_24:
            dst->format = ms::TextureFormat::RGBu8;
            data.assign(image, image + (num_pixels * 3));
            break;
        case kFBVideoFormat_BGR_24:
            dst->format = ms::TextureFormat::RGBu8;
            data.resize_discard(num_pixels * 3);
            mu::BGR2RGB((mu::unorm8x3*)data.data(), (mu::unorm8x3*)image, num_pixels);
            break;
        default:
            // not supported
            dst->format = ms::TextureFormat::RGBAu8;
            data.resize_zeroclear(num_pixels * 4);
            break;
        }
    }

    return dst->id;
}

bool msmbDevice::exportMaterial(FBMaterial* src)
{
    if (!src)
        return false;

    auto& rec = m_material_records[src];
    if (rec.dst)
        return rec.id; // already exported

    auto dst = ms::Material::create();
    m_materials.push_back(dst);
    rec.dst = dst.get();
    if (rec.id == -1)
        rec.id = (int)m_material_records.size() - 1;

    dst->id = rec.id;
    dst->name = src->LongName;
    dst->setColor(to_float4(src->Diffuse));

    auto emissive = to_float4(src->Emissive);
    if ((mu::float3&)emissive != mu::float3::zero())
        dst->setEmission(emissive);

    if (m_dirty_textures) {
        dst->setColorMap(exportTexture(src->GetTexture(kFBMaterialTextureDiffuse), kFBMaterialTextureDiffuse));
        dst->setEmissionMap(exportTexture(src->GetTexture(kFBMaterialTextureEmissive), kFBMaterialTextureEmissive));
        dst->setNormalMap(exportTexture(src->GetTexture(kFBMaterialTextureNormalMap), kFBMaterialTextureNormalMap));
    }
    return true;
}


bool msmbDevice::sendAnimations()
{
    // wait for previous request to complete
    if (m_future_send.valid()) {
        m_future_send.get();
    }

    if (exportAnimations()) {
        kickAsyncSend();
        return true;
    }
    else {
        return false;
    }
}

bool msmbDevice::exportAnimations()
{
    auto& system = FBSystem::TheOne();
    FBPlayerControl control;

    // create default clip
    m_animations.push_back(ms::AnimationClip::create());

    // gather models
    int num_animations = 0;
    EnumerateAllNodes([this, &num_animations](FBModel *node) {
        if (exportAnimation(node, false))
            ++num_animations;
    });
    if (num_animations == 0)
        return false;


    FBTime time_current = system.LocalTime;
    double time_begin, time_end;
    std::tie(time_begin, time_end) = GetTimeRange(system.CurrentTake);
    double interval = 1.0 / std::max(samples_per_second, 1.0f);

    int reserve_size = int((time_end - time_begin) / interval) + 1;
    for (auto& kvp : m_anim_records) {
        kvp.second.dst->reserve(reserve_size);
    }

    // advance frame and record
    for (double t = time_begin; t < time_end; t += interval) {
        FBTime fbt;
        fbt.SetSecondDouble(t);
        control.Goto(fbt);
        m_anim_time = (float)t;
        for (auto& kvp : m_anim_records)
            kvp.second(this);
    }

    // cleanup
    m_anim_records.clear();
    control.Goto(time_current);

    // keyframe reduction
    for (auto& clip : m_animations)
        clip->reduction();

    // erase empty clip
    m_animations.erase(
        std::remove_if(m_animations.begin(), m_animations.end(), [](ms::AnimationClipPtr& p) { return p->empty(); }),
        m_animations.end());

    return !m_animations.empty();
}

bool msmbDevice::exportAnimation(FBModel *src, bool force)
{
    if (!src || m_anim_records.find(src) != m_anim_records.end())
        return 0;

    ms::AnimationPtr dst;
    AnimationRecord::extractor_t extractor = nullptr;

    if (IsCamera(src)) { // camera
        exportAnimation(src->Parent, true);
        dst = ms::CameraAnimation::create();
        extractor = &msmbDevice::extractCameraAnimation;
    }
    else if (IsLight(src)) { // light
        exportAnimation(src->Parent, true);
        dst = ms::LightAnimation::create();
        extractor = &msmbDevice::extractLightAnimation;
    }
    else if (IsBone(src) || IsMesh(src) || force) { // other
        exportAnimation(src->Parent, true);
        dst = ms::TransformAnimation::create();
        extractor = &msmbDevice::extractTransformAnimation;
    }

    if (dst) {
        auto& rec = m_anim_records[src];
        rec.src = src;
        rec.dst = dst.get();
        rec.extractor = extractor;
        m_animations.front()->animations.push_back(dst);
        return true;
    }
    else {
        return false;
    }
}

void msmbDevice::extractTransformAnimation(ms::Animation& dst_, FBModel* src)
{
    auto pos = mu::float3::zero();
    auto rot = mu::quatf::identity();
    auto scale = mu::float3::one();
    bool vis = true;
    ExtractTransformData(src, pos, rot, scale, vis);

    float t = m_anim_time * time_scale;
    auto& dst = (ms::TransformAnimation&)dst_;
    dst.translation.push_back({ t, pos });
    dst.rotation.push_back({ t, rot });
    dst.scale.push_back({ t, scale });
    //dst.visible.push_back({ t, vis });

    dst.path = GetPath(src);
}

void msmbDevice::extractCameraAnimation(ms::Animation& dst_, FBModel* src)
{
    extractTransformAnimation(dst_, src);

    auto& dst = static_cast<ms::CameraAnimation&>(dst_);
    {
        auto& last = dst.rotation.back();
        last.value *= mu::rotateY(90.0f * mu::Deg2Rad);
    }

    bool ortho;
    float near_plane, far_plane, fov, horizontal_aperture, vertical_aperture, focal_length, focus_distance;
    ExtractCameraData(static_cast<FBCamera*>(src), ortho, near_plane, far_plane, fov, horizontal_aperture, vertical_aperture, focal_length, focus_distance);

    float t = m_anim_time * time_scale;
    dst.near_plane.push_back({ t , near_plane });
    dst.far_plane.push_back({ t , far_plane });
    dst.fov.push_back({ t , fov });
}

void msmbDevice::extractLightAnimation(ms::Animation& dst_, FBModel* src)
{
    extractTransformAnimation(dst_, src);

    auto& dst = static_cast<ms::LightAnimation&>(dst_);
    {
        auto& last = dst.rotation.back();
        last.value *= mu::rotateX(90.0f * mu::Deg2Rad);
    }

    ms::Light::LightType type;
    mu::float4 color;
    float intensity;
    float spot_angle;
    ExtractLightData(static_cast<FBLight*>(src), type, color, intensity, spot_angle);

    float t = m_anim_time * time_scale;
    dst.color.push_back({ t, color });
    dst.intensity.push_back({ t, intensity });
    if (type == ms::Light::LightType::Spot)
        dst.spot_angle.push_back({ t, spot_angle });
}

void msmbDevice::AnimationRecord::operator()(msmbDevice *_this)
{
    (_this->*extractor)(*dst, src);
}
