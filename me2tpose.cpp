/* me2tpose is distributed under MIT license:
 * 
 * Copyright (c) 2021 Kota Iguchi
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>

#define CGLTF_IMPLEMENTATION
#define CGLTF_WRITE_IMPLEMENTATION

#include "cgltf_write.h"
#include "CLI11.hpp"
#include "json11.hpp"

#define GLM_FORCE_RADIANS
#define GLM_FORCE_SILENT_WARNINGS
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/matrix_decompose.hpp>

#define RETURN_WITH_ERROR(MSG, DATA)             \
    std::cout << "[ERROR] " << MSG << std::endl; \
    cgltf_free(DATA);                            \
    return 1;

static void f3_min(cgltf_float* a, cgltf_float* b, cgltf_float* out)
{
    out[0] = a[0] < b[0] ? a[0] : b[0];
    out[1] = a[1] < b[1] ? a[1] : b[1];
    out[2] = a[2] < b[2] ? a[2] : b[2];
}

static void f3_max(cgltf_float* a, cgltf_float* b, cgltf_float* out)
{
    out[0] = a[0] > b[0] ? a[0] : b[0];
    out[1] = a[1] > b[1] ? a[1] : b[1];
    out[2] = a[2] > b[2] ? a[2] : b[2];
}

static void write_bin(cgltf_data* data, std::string output)
{
    std::ofstream fout;
    fout.open(output.c_str(), std::ios::out | std::ios::binary);

    for (cgltf_size i = 0; i < data->buffers_count; ++i) {
        cgltf_buffer* buffer = &data->buffers[i];

        cgltf_size aligned_size = (buffer->size + 3) & ~3;
        cgltf_size align = aligned_size - buffer->size;

        fout.write(reinterpret_cast<const char*>(&aligned_size), 4);
        fout.write(reinterpret_cast<const char*>(&GlbMagicBinChunk), 4);
        fout.write(reinterpret_cast<const char*>(buffer->data), buffer->size);

        for (uint32_t j = 0; j < align; ++j) {
            fout.write(" ", 1);
        }
    }

    fout.close();
}

static void write(std::string output, std::string in_json, std::string in_bin)
{
    std::ifstream in_json_st(in_json, std::ios::in | std::ios::binary);
    std::ifstream in_bin_st(in_bin, std::ios::in | std::ios::binary);
    std::ofstream out_st(output, std::ios::trunc | std::ios::binary);

    in_json_st.seekg(0, std::ios::end);
    uint32_t json_size = (uint32_t)in_json_st.tellg();
    in_json_st.seekg(0, std::ios::beg);

    uint32_t aligned_json_size = (json_size + 3) & ~3;
    uint32_t json_align = aligned_json_size - json_size;

    in_bin_st.seekg(0, std::ios::end);
    uint32_t bin_size = (uint32_t)in_bin_st.tellg();
    in_bin_st.seekg(0, std::ios::beg);

    uint32_t total_size = GlbHeaderSize + GlbChunkHeaderSize + aligned_json_size + bin_size;

    out_st.write(reinterpret_cast<const char*>(&GlbMagic), 4);
    out_st.write(reinterpret_cast<const char*>(&GlbVersion), 4);
    out_st.write(reinterpret_cast<const char*>(&total_size), 4);

    out_st.write(reinterpret_cast<const char*>(&aligned_json_size), 4);
    out_st.write(reinterpret_cast<const char*>(&GlbMagicJsonChunk), 4);

    out_st << in_json_st.rdbuf();
    for (uint32_t i = 0; i < json_align; ++i) {
        out_st << ' ';
    }
    out_st << in_bin_st.rdbuf();

    out_st.close();
}

static inline glm::mat4 node_transform(const cgltf_node* node)
{
    auto matrix = glm::mat4(1.0f);
    auto translation = glm::make_vec3(node->translation);
    auto scale = glm::make_vec3(node->scale);
    auto rotation = glm::make_quat(node->rotation);

    return glm::translate(glm::mat4(1.0f), translation) * glm::mat4(rotation) * glm::scale(glm::mat4(1.0f), scale) * matrix;
}

static inline glm::mat4 bind_translation(const cgltf_node* node)
{
    auto m = node_transform(node);
    cgltf_node* parent = node->parent;
    while (parent != nullptr) {
        m = node_transform(parent) * m;
        parent = parent->parent;
    }

    return m;
}

static void transform_apply(cgltf_node* node, const glm::mat4 parent_matrix)
{
    const glm::mat4 identity = glm::mat4(1.0f);

    const glm::vec3 pos = glm::make_vec3(node->translation);
    const glm::vec3 scale = glm::make_vec3(node->scale);
    const glm::quat rot = glm::make_quat(node->rotation);

    glm::mat4 self = glm::translate(glm::mat4(1.0f), pos) * glm::mat4(rot) * glm::scale(glm::mat4(1.0f), scale) * identity;
    glm::mat4 bind_matrix = parent_matrix * self;

    glm::quat rot_parent;
    glm::vec3 scale_parent, pos_parent, skew_unused;
    glm::vec4 perspective_unused;
    glm::decompose(parent_matrix, scale_parent, rot_parent, pos_parent, skew_unused, perspective_unused);

    glm::vec3 newpos = rot_parent * pos;

    node->translation[0] = newpos.x * scale_parent.x;
    node->translation[1] = newpos.y * scale_parent.y;
    node->translation[2] = newpos.z * scale_parent.z;
    node->rotation[0] = 0;
    node->rotation[1] = 0;
    node->rotation[2] = 0;
    node->rotation[3] = 1;
    node->scale[0] = 1;
    node->scale[1] = 1;
    node->scale[2] = 1;

    for (cgltf_size i = 0; i < node->children_count; i++) {
        transform_apply(node->children[i], bind_matrix);
    }
}

static bool apply_weight(cgltf_node* skin_node, cgltf_float* positions, cgltf_uint* joints, cgltf_float* weights, cgltf_float* normals)
{
    glm::mat4 skinMat = glm::mat4(0.f);

    const cgltf_skin* skin = skin_node->skin;

    const cgltf_accessor* accessor = skin_node->skin->inverse_bind_matrices;
    uint8_t* ibm_data = (uint8_t*)accessor->buffer_view->buffer->data + accessor->buffer_view->offset + accessor->offset;

    glm::mat4 globalTransform = bind_translation(skin_node);
    glm::mat4 globalInverseTransform = glm::inverse(globalTransform);

    for (cgltf_size i = 0; i < 4; ++i) {
        const cgltf_uint joint_index = joints[i];
        cgltf_float weight = weights[i];

        if (weight <= 0)
            continue;

        if (skin->joints_count <= joint_index)
            continue;

        const cgltf_float* ibm_mat = (cgltf_float*)(ibm_data + accessor->stride * joint_index);

        const auto joint = skin->joints[joint_index];

        glm::mat4 globalTransformOfJointNode = bind_translation(joint);
        glm::mat4 inverseBindMatrixForJoint = glm::make_mat4(ibm_mat);
        glm::mat4 jointMatrix = globalTransformOfJointNode * inverseBindMatrixForJoint;
        jointMatrix = globalInverseTransform * jointMatrix;

        skinMat += jointMatrix * weight;
    }

    glm::vec4 inPos = glm::vec4(positions[0], positions[1], positions[2], 1.f);
    glm::vec4 locPos = globalTransform * skinMat * inPos;

    positions[0] = locPos.x;
    positions[1] = locPos.y;
    positions[2] = locPos.z;

    if (normals != nullptr) {
        glm::vec3 inNormal = glm::make_vec3(normals);
        glm::vec3 outNormal = glm::normalize(glm::transpose(glm::inverse(glm::mat3(globalTransform * skinMat))) * inNormal);

        normals[0] = outNormal.x;
        normals[1] = outNormal.y;
        normals[2] = outNormal.z;
    }

    return true;
}

static bool apply_weights(cgltf_node* skin_node, cgltf_accessor* positions, cgltf_accessor* joints, cgltf_accessor* weights, cgltf_accessor* normals)
{
    cgltf_uint* joints_data = (cgltf_uint*)calloc(joints->count * 4, sizeof(cgltf_uint));
    for (cgltf_size i = 0; i < joints->count; ++i) {
        cgltf_accessor_read_uint(joints, i, joints_data + (i * 4), 4);
    }

    cgltf_size unpack_count = weights->count * 4;
    cgltf_float* weights_data = (cgltf_float*)calloc(unpack_count, sizeof(cgltf_float));
    cgltf_accessor_unpack_floats(weights, weights_data, unpack_count);

    uint8_t* positions_data = (uint8_t*)positions->buffer_view->buffer->data + positions->buffer_view->offset + positions->offset;
    uint8_t* normals_data = nullptr;

    if (normals != nullptr) {
        normals_data = (uint8_t*)normals->buffer_view->buffer->data + normals->buffer_view->offset + normals->offset;
    }

    for (cgltf_size i = 0; i < positions->count; ++i) {
        cgltf_float* position = (cgltf_float*)(positions_data + (positions->stride * i));
        cgltf_float* normal = normals_data != nullptr ? (cgltf_float*)(normals_data + (normals->stride * i)) : nullptr;

        apply_weight(skin_node, position, joints_data + (i * 4), weights_data + (i * 4), normal);
    }
    return true;
}

// A-pose to T-pose
static void pose(cgltf_data* data, json11::Json::object items)
{
    for (cgltf_size i = 0; i < data->nodes_count; ++i) {
        const auto node = &data->nodes[i];
        if (node->name == nullptr)
            continue;
        const auto iter = items.find(node->name);
        if (iter != items.end()) {
            const auto values = iter->second.array_items();
            node->rotation[0] = (cgltf_float)values[0].number_value();
            node->rotation[1] = (cgltf_float)values[1].number_value();
            node->rotation[2] = (cgltf_float)values[2].number_value();
            node->rotation[3] = (cgltf_float)values[3].number_value();
        }
    }


}

static bool skinning(cgltf_data* data, std::string config_file)
{
    std::ifstream f(config_file, std::ios::in);
    if (f.fail()) {
        std::cout << "[ERROR] failed to open Bone config file " << config_file << std::endl;
        return false;
    }

    std::ostringstream ss;
    ss << f.rdbuf();
    std::string content = ss.str();

    std::string parse_error;
    const auto json = json11::Json::parse(content, parse_error);

    if (!parse_error.empty()) {
        std::cout << "[ERROR] failed to parse config file " << config_file << std::endl;
        std::cout << "\t" << parse_error << std::endl;
        return false;
    }

    const auto& items = json.object_items();

    for (const auto item : items) {
        const auto& key = item.first;
        if (key == "pose") {
            pose(data, item.second.object_items());
        }
    }

    // skinning
    for (cgltf_size i = 0; i < data->nodes_count; ++i) {
        const auto node = &data->nodes[i];
        const auto mesh = node->mesh;
        if (mesh == nullptr)
            continue;
        for (cgltf_size j = 0; j < mesh->primitives_count; ++j) {
            const auto primitive = &mesh->primitives[j];
            cgltf_accessor* acc_POSITION = nullptr;
            cgltf_accessor* acc_JOINTS = nullptr;
            cgltf_accessor* acc_WEIGHTS = nullptr;
            cgltf_accessor* acc_NORMAL = nullptr;
            for (cgltf_size k = 0; k < primitive->attributes_count; ++k) {
                const auto attr = &primitive->attributes[k];
                if (attr->type == cgltf_attribute_type_position) {
                    acc_POSITION = attr->data;
                } else if (attr->type == cgltf_attribute_type_normal) {
                    acc_NORMAL = attr->data;
                } else if (attr->type == cgltf_attribute_type_joints) {
                    acc_JOINTS = attr->data;
                } else if (attr->type == cgltf_attribute_type_weights) {
                    acc_WEIGHTS = attr->data;
                }
            }
            if (acc_POSITION && acc_JOINTS && acc_WEIGHTS) {
                apply_weights(node, acc_POSITION, acc_JOINTS, acc_WEIGHTS, acc_NORMAL);
            }
        }
    }
    return true;
}

static inline void apply(cgltf_data* data)
{
    for (cgltf_size i = 0; i < data->scenes_count; ++i) {
        glm::mat4 identity = glm::mat4(1.f);
        const auto scene = &data->scenes[i];
        for (cgltf_size j = 0; j < scene->nodes_count; ++j) {
            transform_apply(scene->nodes[j], identity);
        }
    }

    std::set<cgltf_accessor*> skin_done;
    for (cgltf_size i = 0; i < data->skins_count; ++i) {
        const auto skin = &data->skins[i];
        const auto accessor = skin->inverse_bind_matrices;

        if (skin_done.count(accessor) > 0) {
            continue;
        }
        skin_done.emplace(accessor);

        uint8_t* buffer_data = (uint8_t*)accessor->buffer_view->buffer->data + accessor->buffer_view->offset + accessor->offset;

        accessor->max[12] = -FLT_MAX;
        accessor->max[13] = -FLT_MAX;
        accessor->max[14] = -FLT_MAX;
        accessor->min[12] = FLT_MAX;
        accessor->min[13] = FLT_MAX;
        accessor->min[14] = FLT_MAX;

        for (cgltf_size j = 0; j < skin->joints_count; ++j) {
            cgltf_node* node = skin->joints[j];
            cgltf_float* inverse_bind_matrix = (cgltf_float*)(buffer_data + accessor->stride * j);

            glm::mat4 inversed = glm::inverse(bind_translation(node));

            inverse_bind_matrix[0] = inversed[0][0];
            inverse_bind_matrix[1] = inversed[0][1];
            inverse_bind_matrix[2] = inversed[0][2];
            inverse_bind_matrix[3] = inversed[0][3];
            inverse_bind_matrix[4] = inversed[1][0];
            inverse_bind_matrix[5] = inversed[1][1];
            inverse_bind_matrix[6] = inversed[1][2];
            inverse_bind_matrix[7] = inversed[1][3];
            inverse_bind_matrix[8] = inversed[2][0];
            inverse_bind_matrix[9] = inversed[2][1];
            inverse_bind_matrix[10] = inversed[2][2];
            inverse_bind_matrix[11] = inversed[2][3];
            inverse_bind_matrix[12] = inversed[3][0];
            inverse_bind_matrix[13] = inversed[3][1];
            inverse_bind_matrix[14] = inversed[3][2];
            inverse_bind_matrix[15] = inversed[3][3];

            f3_max(inverse_bind_matrix + 12, accessor->max + 12, accessor->max + 12);
            f3_min(inverse_bind_matrix + 12, accessor->min + 12, accessor->min + 12);
        }
    }
}

int main(int argc, char** argv)
{
    CLI::App app { "me2tpose: T-pose Ready Player Me avatar glTF binary (.glb)" };

    std::string input;
    app.add_option("-i,--input", input, "input A-pose glTF binary (.glb) file name");

    std::string config_file = "config.json";
    app.add_option("-c,--config", config_file, "Bone configuration (JSON)");

    CLI11_PARSE(app, argc, argv);

    cgltf_options options = {};
    cgltf_data* data = NULL;
    cgltf_result result = cgltf_parse_file(&options, input.c_str(), &data);

    if (result != cgltf_result_success) {
        std::cout << "[ERROR] failed to parse file " << input << std::endl;
        return result;
    }

    result = cgltf_load_buffers(&options, data, input.c_str());

    if (result != cgltf_result_success) {
        RETURN_WITH_ERROR("failed to load buffers. Exiting.", data)
    }

    std::cout << "[INFO] Converting " << input << std::endl;
    if (skinning(data, config_file)) {
        apply(data);
    } else {
        RETURN_WITH_ERROR("failed to do skinning. Exiting.", data)
    }

    result = cgltf_validate(data);

    if (result != cgltf_result_success) {
        std::cout << "[WARN] Invalid glTF data: " << result << std::endl;
    }

    std::string output = input + ".glb";
    std::string out_json = input + ".json";
    std::string out_bin = input + ".bin";

    cgltf_write_file(&options, out_json.c_str(), data);

    write_bin(data, out_bin);
    write(output, out_json, out_bin);

    cgltf_free(data);

    std::cout << "[INFO] Successfully converted to " << output << std::endl;

    return cgltf_result_success;
}
