#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {
// Steam app 32440, build 1017673. Addresses were checked in the live x86 image.
constexpr uintptr_t render_call = 0x49356b;
constexpr uintptr_t input_angle = 0x54b3d0;
constexpr uintptr_t scene_loop = 0x60b1d4;
constexpr uintptr_t scene_count = 0x95fbb8;
constexpr uintptr_t scene_array = 0x9640f0;
constexpr uintptr_t camera_pointer = 0x925164;
constexpr uintptr_t player_array = 0x93d7f0;
constexpr uintptr_t world_pointer = 0x802c54;
constexpr uintptr_t object_count = 0x93d500;
constexpr uintptr_t object_array = 0x93d504;
constexpr uintptr_t automatic_dropout = 0x4288f9;
constexpr uintptr_t pickup_camera_load = 0x56f345;
constexpr uintptr_t force_target_clip = 0x468d20;
constexpr uintptr_t sustain_force_clip = 0x4a214b;
constexpr uintptr_t offscreen_bolt = 0x581e14;
constexpr uintptr_t player_trigger_node = 0x514e70;
constexpr uintptr_t player_trigger_area = 0x514fb0;
constexpr size_t object_size = 0x10d8;
constexpr size_t max_objects = 64;
constexpr size_t room_visibility = 0x2991;
constexpr size_t model_draw_result = 0x288;
constexpr uintptr_t frame_time = 0x925018;
constexpr uintptr_t paused = 0x93b328;
constexpr uintptr_t menu_depth = 0x800944;
constexpr uintptr_t first_menu_index = 0x921454;
constexpr uintptr_t graphics_backend = 0x29765c8;
constexpr uintptr_t game_window = 0x924864;
constexpr uintptr_t game_keyboard = 0x29765a4;
constexpr uintptr_t keyboard_cooperative_call = 0x6da138;
constexpr size_t menu_size = 0xe0;
constexpr int stranger_customizer_index = 43; // Menu ID 12 in the Steam menu table.
constexpr float tau = 6.28318530718f;
constexpr unsigned char render_bytes[] = {0xe8, 0x80, 0xf2, 0xff, 0xff};
constexpr unsigned char angle_bytes[] = {0x8b,0x4c,0x24,0x04,0xf6,0x81,0xfc,0x01,0x00,0x00,0x80};
constexpr unsigned char scene_bytes[] = {0xd9,0xe8,0x8b,0x56,0xec};
constexpr unsigned char dropout_bytes[] = {0xd9,0x05,0x34,0x67,0x7f,0x00};
constexpr unsigned char pickup_camera_bytes[] = {0xa1,0xb4,0x21,0x80,0x00};
constexpr unsigned char sustain_force_bytes[] = {0xd9,0xee,0xd8,0x9e,0x64,0x0c,0x00,0x00};
constexpr unsigned char offscreen_bolt_bytes[] = {0xc6,0x45,0x38,0x00,0xeb,0x04};
constexpr unsigned char player_node_bytes[] = {0x8b,0x4c,0x24,0x04,0x85,0xc9};
constexpr unsigned char player_trigger_bytes[] = {0x8b,0x44,0x24,0x04,0x85,0xc0};
constexpr unsigned char keyboard_cooperative_bytes[] = {0x8b,0x41,0x34,0xff,0xd0};

struct Vec { float x, y, z; };
struct Matrix {
    float m[4][4];
    float component(Vec point, int column) const {
        return point.x * m[0][column] + point.y * m[1][column] + point.z * m[2][column] + m[3][column];
    }
    Vec transform(Vec point) const {
        return {component(point, 0), component(point, 1), component(point, 2)};
    }
};
struct Camera {
    Matrix matrix;
    float fov, aspect, near_clip, far_clip;
    float other[10];
};
static_assert(sizeof(Camera) == 0x78);

extern "C" {
    Camera* volatile RenderViewCamera __attribute__((used)) = nullptr;
    unsigned volatile KeptOffscreenBolts __attribute__((used)) = 0;
}

struct Frustum {
    Matrix view, clip;
    float near_clip, far_clip;

    bool contains(Vec center, float radius) const {
        center = view.transform(center);
        if (center.z + radius < near_clip || center.z - radius > far_clip) return false;
        for (int side = 0; side < 4; ++side) {
            if (clip.component(center, side) < -radius) return false;
        }
        return true;
    }
};
Frustum force_views[2];

struct Orbit {
    float yaw = 0, pitch = 0.24f;
    float roof_check_timer = 0, open_air_blend = 0;
    float boom_fraction = 1.0f, boom_clear_time = 0.0f;
    bool open_air = false;
    bool checked_roof = false;
};
Orbit orbit[2];
Vec view_eye[2];
void* active_world;
bool second_view;
using AngleFn = uint16_t (__cdecl *)(const void*, const void*);
AngleFn original_angle;
bool reported_split;
bool reported_error;
bool reported_auto_dropout;
bool reported_pickup_merge;
bool reported_force_target;
bool reported_sustained_force;
bool reported_combat;
bool reported_player_room;
bool reported_split_hint;
bool reported_split_studs[2];
bool flushing_first_view;

template<class T> T& at(uintptr_t address) { return *reinterpret_cast<T*>(address); }
template<class T> const T& member(const void* object, unsigned offset) {
    return *reinterpret_cast<const T*>(static_cast<const unsigned char*>(object) + offset);
}
template<class Fn> Fn fn(uintptr_t address) { return reinterpret_cast<Fn>(address); }

void log(const char* message) {
    wchar_t folder[MAX_PATH];
    DWORD length = GetTempPathW(MAX_PATH, folder);
    if (!length || length + 32 >= MAX_PATH) return;
    wcscat(folder, L"TCS-Split-Prototype.log");
    HANDLE file = CreateFileW(folder, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written;
    WriteFile(file, message, static_cast<DWORD>(strlen(message)), &written, nullptr);
    WriteFile(file, "\r\n", 2, &written, nullptr);
    CloseHandle(file);
}

const void* player(int slot) { return at<const void*>(player_array + slot * 4); }

bool both_players_joined() {
    for (int slot = 0; slot < 2; ++slot) {
        const void* object = player(slot);
        if (!object || !(member<unsigned char>(object, 0x1fc) & 0x80)) return false;
    }
    return true;
}

bool local_free_roam(const void* world) {
    if (!world || at<int>(0x951288)) return false;
    const void* level = member<const void*>(world, 0x128);
    if (!level || level == at<void*>(0x951b88) || level == at<void*>(0x951b98)
        || level == at<void*>(0x951b9c) || (member<unsigned>(level, 0x64) & 0x400))
        return false;
    return both_players_joined();
}

bool stranger_customizer_open() {
    // Keep the shared view if the customizer opens a save dialog above itself.
    for (int slot = std::min(at<int>(menu_depth), 9); slot > 0; --slot) {
        if (at<int16_t>(first_menu_index + slot * menu_size) == stranger_customizer_index) return true;
    }
    return false;
}

bool eligible(const void* world) {
    return local_free_roam(world) && !at<int>(0x951be0) && !at<void*>(0x951c34)
        && !at<int>(0x855de0) && at<float>(0x950678) == 1.0f
        && at<Camera*>(camera_pointer) && !stranger_customizer_open();
}

struct CompanionAssignment {
    const void* character = nullptr;
    uint16_t id = 0;
    int slot = 0;
};
std::array<CompanionAssignment, 8> companion_assignments;
const void* companion_world;
const void* companion_level;
const void* companion_pool;
bool reported_companion;
bool reported_player_trigger;

bool split_companions() {
    const void* world = at<const void*>(world_pointer);
    if (!world) return false;
    const void* level = member<const void*>(world, 0x128);
    const void* pool = at<const void*>(object_array);
    if (companion_world != world || companion_level != level || companion_pool != pool) {
        companion_assignments = {};
        companion_world = world;
        companion_level = level;
        companion_pool = pool;
    }
    return world == active_world && local_free_roam(world);
}

int __cdecl TagPartyCharacter(void* source, void* target, int takeover, int blend, int mode) {
    const bool remember = takeover == 0 && split_companions();
    const int owner = !remember ? -1 : source == player(0) ? 0 : source == player(1) ? 1 : -1;
    const int result = fn<decltype(&TagPartyCharacter)>(0x4876a0)(source, target, takeover, blend, mode);
    if (owner < 0 || result != 1 || target != player(owner)) return result;
    for (auto& assignment : companion_assignments) {
        if (assignment.character == source || assignment.character == target) assignment = {};
    }
    const unsigned party_slot = member<unsigned char>(source, 0x280);
    if (party_slot < companion_assignments.size() && source == player(party_slot)
        && !(member<unsigned char>(source, 0x1fc) & 0x80)) {
        companion_assignments[party_slot] = {source, member<uint16_t>(source, 0x1064), owner};
    }
    return result;
}

void __cdecl FollowPartyOwner(const void* companion, const void* target, int flags, float distance) {
    if (companion && split_companions()) {
        const unsigned party_slot = member<unsigned char>(companion, 0x280);
        if (party_slot < companion_assignments.size() && companion == player(party_slot)
            && !(member<unsigned char>(companion, 0x1fc) & 0x80)) {
            const auto& assignment = companion_assignments[party_slot];
            if (assignment.character == companion && assignment.id == member<uint16_t>(companion, 0x1064)) {
                const void* owner = player(assignment.slot);
                if (target != owner && !reported_companion) {
                    reported_companion = true;
                    log("Kept a switched-out companion with the player who last controlled them.");
                }
                target = owner;
            }
        }
    }
    fn<decltype(&FollowPartyOwner)>(0x534e10)(companion, target, flags, distance);
}

extern "C" __attribute__((naked)) float __cdecl StockPlayerInTriggerArea(void*, void*, void*, const char*, void*) {
    asm volatile("mov 4(%esp), %eax; test %eax, %eax; jmp *1f; 1: .long 0x514fb6");
}

extern "C" __attribute__((naked)) float __cdecl StockPlayerInLevelNode(void*, void*, void*, const char*, void*) {
    asm volatile("mov 4(%esp), %ecx; test %ecx, %ecx; jmp *1f; 1: .long 0x514e76");
}

float __cdecl EitherLocalPlayerInTriggerArea(void* ai, void* script, void* packet, const char* name, void* area) {
    const float first = StockPlayerInTriggerArea(ai, script, packet, name, area);
    if (first != 0.0f || !local_free_roam(at<void*>(world_pointer))) return first;
    const float second = fn<decltype(&EitherLocalPlayerInTriggerArea)>(0x515010)(ai, script, packet, name, area);
    if (second != 0.0f && !reported_player_trigger) {
        reported_player_trigger = true;
        log("Player 2 satisfied a shared scripted area trigger.");
    }
    return second;
}

float __cdecl EitherLocalPlayerInLevelNode(void* ai, void* script, void* packet, const char* name, void* node) {
    if (local_free_roam(at<void*>(world_pointer)))
        return fn<decltype(&EitherLocalPlayerInLevelNode)>(0x514ec0)(ai, script, packet, name, node);
    return StockPlayerInLevelNode(ai, script, packet, name, node);
}

int __cdecl ForceTargetClip(const Vec* point, float radius, const Matrix* matrix) {
    const auto* world = at<const void*>(world_pointer);
    if (world != active_world || !eligible(world))
        return fn<int (__cdecl *)(const Vec*, float, const Matrix*)>(0x5d6620)(point, radius, matrix);
    const Vec center = matrix ? matrix->transform(*point) : *point;
    const bool p1 = force_views[0].contains(center, radius);
    const bool p2 = force_views[1].contains(center, radius);
    if (p2 && !p1 && !reported_force_target) {
        reported_force_target = true;
        log("Made a Force target available from Player 2's view.");
    }
    return p1 || p2 ? 0 : 1;
}

struct CharacterDrawState {
    std::array<Matrix, 16> locators;
    uint32_t flags_e20, flags_e24;
    unsigned char visible, ready;
};
std::array<CharacterDrawState, max_objects> first_character_draw;
unsigned char* first_character_objects;
int first_character_count;

void capture_character_draw() {
    first_character_objects = at<unsigned char*>(object_array);
    first_character_count = std::clamp(at<int>(object_count), 0, static_cast<int>(max_objects));
    for (int i = 0; first_character_objects && i < first_character_count; ++i) {
        const auto* object = first_character_objects + i * object_size;
        auto& draw = first_character_draw[i];
        draw.visible = member<unsigned char>(object, model_draw_result);
        draw.ready = member<unsigned char>(object, 0x28c);
        draw.flags_e20 = member<uint32_t>(object, 0xe20);
        draw.flags_e24 = member<uint32_t>(object, 0xe24);
        if (draw.visible && draw.ready)
            std::memcpy(draw.locators.data(), object + 0x804, sizeof(draw.locators));
    }
}

void merge_character_draw() {
    if (!first_character_objects || first_character_objects != at<unsigned char*>(object_array)
        || first_character_count != at<int>(object_count)) return;
    for (int i = 0; i < first_character_count; ++i) {
        const uintptr_t object = reinterpret_cast<uintptr_t>(first_character_objects + i * object_size);
        const auto& first = first_character_draw[i];
        auto& visible = at<unsigned char>(object + model_draw_result);
        auto& ready = at<unsigned char>(object + 0x28c);
        auto& flags_e20 = at<uint32_t>(object + 0xe20);
        auto& flags_e24 = at<uint32_t>(object + 0xe24);
        if (visible && ready && !first.visible && at<signed char>(object + 0xe3f) != -1 && !reported_combat) {
            reported_combat = true;
            log("Processed a queued blaster shot from a character drawn only in Player 2's view.");
        }
        if (first.visible && first.ready && (!visible || !ready || (!(flags_e24 & 8) && (first.flags_e24 & 8)))) {
            std::memcpy(reinterpret_cast<void*>(object + 0x804), first.locators.data(), sizeof(first.locators));
            visible = first.visible;
            ready = first.ready;
            flags_e24 = (flags_e24 & ~8u) | (first.flags_e24 & 8);
        }
        // The first pass saw the previous frame. The second pass saw the first pass.
        flags_e20 = (flags_e20 & ~0x08000000u) | (first.flags_e20 & 0x08000000u);
    }
}

struct WorldVisibility {
    std::array<unsigned char, 256> rooms{};
    std::array<unsigned char, max_objects> models{};
    unsigned char* objects;
    int count;

    explicit WorldVisibility(const void* world)
        : objects(at<unsigned char*>(object_array)),
          count(std::clamp(at<int>(object_count), 0, static_cast<int>(max_objects))) {
        std::memcpy(rooms.data(), static_cast<const unsigned char*>(world) + room_visibility, rooms.size());
        for (int i = 0; objects && i < count; ++i)
            models[i] = objects[i * object_size + model_draw_result];
    }

    void merge_into(void* world) const {
        auto* visible = static_cast<unsigned char*>(world) + room_visibility;
        for (size_t i = 0; i < rooms.size(); ++i) visible[i] |= rooms[i];
        if (!objects || objects != at<unsigned char*>(object_array) || count != at<int>(object_count))
            return;
        for (int i = 0; i < count; ++i)
            objects[i * object_size + model_draw_result] |= models[i];
    }
};

struct PickupRuntime {
    unsigned char* placed;
    unsigned char* temporary;
    int count;
};

struct PickupVisibility {
    static constexpr int stride = 0x2c;
    static constexpr int flags_offset = 0x17;
    static constexpr unsigned char drawn_bit = 0x10;
    unsigned char* records = nullptr;
    std::vector<unsigned char> drawn;

    static unsigned char& flags(unsigned char* records, size_t index) {
        return records[index * stride + flags_offset];
    }

    void capture(unsigned char* list, int count) {
        records = list;
        drawn.resize(list && count > 0 ? count : 0);
        for (size_t i = 0; i < drawn.size(); ++i)
            drawn[i] = flags(list, i) & drawn_bit;
    }

    bool merge(unsigned char* list, int count) const {
        if (!list || list != records || count < 0 || static_cast<size_t>(count) != drawn.size())
            return false;
        bool restored = false;
        for (size_t i = 0; i < drawn.size(); ++i) {
            unsigned char& current = flags(list, i);
            if (drawn[i] && (current & (1 | 8 | drawn_bit)) == 1) {
                current |= drawn_bit;
                restored = true;
            }
        }
        return restored;
    }
};

PickupVisibility placed_pickups, temporary_pickups;

void capture_pickups(const PickupRuntime* pickups) {
    placed_pickups.capture(pickups ? pickups->placed : nullptr, pickups ? pickups->count : 0);
    temporary_pickups.capture(pickups ? pickups->temporary : nullptr, 64);
}

void merge_pickups(const PickupRuntime* pickups) {
    if (!pickups) return;
    const bool placed = placed_pickups.merge(pickups->placed, pickups->count);
    const bool temporary = temporary_pickups.merge(pickups->temporary, 64);
    if ((placed || temporary) && !reported_pickup_merge) {
        reported_pickup_merge = true;
        log("Preserved collectible visibility from both camera views.");
    }
}

extern "C" __attribute__((naked)) void PickupCameraEntry() {
    // Replay the pickup renderer's horizontal distance calculation with the
    // current view's eye. The single-view path still reads the stock camera.
    asm volatile("mov _RenderViewCamera, %eax; test %eax, %eax; jz 1f;"
                 "flds -0x1c(%esi); fsubs 0x30(%eax); lea -0x1c(%esi), %ebx; fstps 0x18(%esp);"
                 "flds -0x14(%esi); fsubs 0x38(%eax); mov $0x56f363, %eax; jmp *%eax;"
                 "1: mov 0x8021b4, %eax; jmp *2f; 2: .long 0x56f34a");
}

void __cdecl DrawForceGlow(Vec* position, float radius, int model, float alpha, const void* owner) {
    if (RenderViewCamera && owner && position == &member<Vec>(owner, 0xc68)
        && (owner == player(0) || owner == player(1)) && owner != player(second_view ? 1 : 0))
        return;
    fn<void (__cdecl *)(Vec*, float, int, float, const void*)>(0x4a4ce0)(position, radius, model, alpha, owner);
}

void __cdecl OrientGlow(Matrix* output, const Matrix* sprite, const Matrix* shared_view) {
    const Camera* camera = RenderViewCamera;
    fn<void (__cdecl *)(Matrix*, const Matrix*, const Matrix*)>(0x5b5700)(
        output, sprite, camera ? &camera->matrix : shared_view);
}

extern "C" __attribute__((naked)) void OffscreenBoltEntry() {
    // A view can hide a bolt, but the shared simulation owns its lifetime.
    asm volatile("cmpl $0, _RenderViewCamera; jz 1f; incl _KeptOffscreenBolts; jmp 2f;"
                 "1: movb $0, 0x38(%ebp); 2: mov $0x581e1e, %eax; jmp *%eax");
}

constexpr uintptr_t part_array = 0x291f664;
constexpr int part_size = 0x220;
const unsigned char* first_parts;
std::vector<unsigned char> first_parts_visible;

void __cdecl DrawPartsForViews(int keep_offscreen) {
    fn<void (__cdecl *)(int)>(0x64f5d0)(keep_offscreen);
    if (!RenderViewCamera || second_view) return;
    first_parts = at<const unsigned char*>(part_array);
    const int count = at<int>(0x81020c);
    first_parts_visible.resize(first_parts && count > 0 ? count : 0);
    for (size_t i = 0; i < first_parts_visible.size(); ++i) {
        const uint32_t flags = member<uint32_t>(first_parts + i * part_size, 0x144);
        first_parts_visible[i] = (flags & 0x201) == 0x201;
    }
}

void __cdecl CullPartFromView(void* part, int impact) {
    if (RenderViewCamera) {
        if (!second_view) return;
        const auto offset = reinterpret_cast<uintptr_t>(part) - reinterpret_cast<uintptr_t>(first_parts);
        const auto index = offset / part_size;
        if (first_parts && first_parts == at<const unsigned char*>(part_array) && index < first_parts_visible.size()
            && first_parts_visible[index]) return;
    }
    fn<void (__cdecl *)(void*, int)>(0x64ac90)(part, impact);
}

float distance_squared(Vec a, Vec b) {
    const Vec delta{a.x - b.x, a.y - b.y, a.z - b.z};
    return delta.x * delta.x + delta.y * delta.y + delta.z * delta.z;
}

void __cdecl UpdateObjectDistances() {
    const auto* world = at<const void*>(world_pointer);
    if (world != active_world || !eligible(world)) {
        fn<void (__cdecl *)()>(0x55d1e0)();
        return;
    }
    auto* object = at<unsigned char*>(object_array);
    const int count = std::clamp(at<int>(object_count), 0, static_cast<int>(max_objects));
    for (int i = 0; object && i < count; ++i, object += object_size) {
        if ((member<uint32_t>(object, 0x1fc) & 0x1001) != 0x1001) continue;
        const Vec position = member<Vec>(object, 0x5c);
        const float nearest = std::min(distance_squared(position, view_eye[0]),
                                       distance_squared(position, view_eye[1]));
        at<float>(reinterpret_cast<uintptr_t>(object) + 0x101c) = std::sqrt(nearest);
    }
}

float stick(unsigned char value) {
    float raw = (value - 127.5f) / 127.5f;
    return std::copysign(std::max(0.0f, std::abs(raw) - 0.18f) / 0.82f, raw);
}

void update_orbit(int slot, bool first_frame) {
    const void* object = player(slot);
    Orbit& camera = orbit[slot];
    if (first_frame) {
        camera = {};
        camera.yaw = member<uint16_t>(object, 0x58) * (tau / 65536.0f);
    }
    const void* pad = member<const void*>(object, 0xca4);
    const void* input = pad ? member<const void*>(pad, 0) : nullptr;
    if (!input || at<int>(paused)) return;
    const float dt = std::clamp(at<float>(frame_time), 0.0f, 0.05f);
    camera.yaw = std::remainder(camera.yaw + 2.8f * dt * stick(member<unsigned char>(input, 0x90)), tau);
    camera.pitch = std::clamp(camera.pitch + 2.0f * dt * stick(member<unsigned char>(input, 0x91)),
                              -0.35f, 1.05f);
}

Camera make_view(Camera camera, int slot, const D3DSURFACE_DESC& desc, LONG height) {
    const void* object = player(slot);
    const Vec origin = member<Vec>(object, 0x5c);
    const float head = std::clamp(member<float>(object, 0xb4) * 0.8f, 0.45f, 2.5f);
    Orbit& follow = orbit[slot];
    const float yaw = follow.yaw, pitch = follow.pitch;
    const float sy = std::sin(yaw), cy = std::cos(yaw);
    const float sp = std::sin(pitch), cp = std::cos(pitch);
    const Vec forward{sy * cp, -sp, cy * cp};
    const Vec right{cy, 0, -sy};
    const Vec up{sy * sp, cp, cy * sp};
    Vec aim{origin.x, origin.y + head, origin.z};
    const auto terrain_cast = fn<int (__cdecl *)(Vec*, Vec*, float, int)>(0x67b280);
    const float dt = at<int>(paused) ? 0.0f : std::clamp(at<float>(frame_time), 0.0f, 0.05f);
    follow.roof_check_timer -= dt;
    if (follow.roof_check_timer <= 0.0f) {
        // A long overhead test preserves the original framing even in tall interiors.
        Vec overhead{0.0f, 30.0f, 0.0f};
        Vec probe_origin = aim;
        follow.open_air = at<void*>(0x292fc68) && !terrain_cast(&probe_origin, &overhead, 0.1f, 0);
        if (!follow.checked_roof) follow.open_air_blend = follow.open_air ? 1.0f : 0.0f;
        follow.checked_roof = true;
        follow.roof_check_timer = 0.12f;
    }
    const float blend_target = follow.open_air ? 1.0f : 0.0f;
    follow.open_air_blend += std::clamp(blend_target - follow.open_air_blend, -4.0f * dt, 4.0f * dt);
    const float distance = 2.8f - 0.2f * follow.open_air_blend;
    const Vec boom{-forward.x * distance, -forward.y * distance, -forward.z * distance};
    Vec clear_boom = boom;
    terrain_cast(&aim, &clear_boom, 0.18f, 0);
    const float length_squared = boom.x * boom.x + boom.y * boom.y + boom.z * boom.z;
    const float clear_fraction = std::clamp((clear_boom.x * boom.x + clear_boom.y * boom.y + clear_boom.z * boom.z)
                                            / length_squared, 0.0f, 1.0f);
    // Doorframes can alternate between a close hit and a clear ray. Use a
    // short run of clear frames before moving the camera away from the player.
    if (clear_fraction < 0.995f && clear_fraction <= follow.boom_fraction + 0.015f)
        follow.boom_clear_time = 0.0f;
    else
        follow.boom_clear_time += dt;
    follow.boom_fraction = std::min(follow.boom_fraction, clear_fraction);
    if (follow.boom_clear_time >= 0.18f)
        follow.boom_fraction = std::min(clear_fraction, follow.boom_fraction + 0.9f * dt);
    const Vec eye{aim.x + boom.x * follow.boom_fraction, aim.y + boom.y * follow.boom_fraction,
                  aim.z + boom.z * follow.boom_fraction};
    view_eye[slot] = eye;
    const float matrix[4][4] = {
        {right.x, right.y, right.z, 0}, {up.x, up.y, up.z, 0},
        {forward.x, forward.y, forward.z, 0}, {eye.x, eye.y, eye.z, 1}
    };
    std::memcpy(&camera.matrix, matrix, sizeof(matrix));
    camera.aspect = float(height) / float(desc.Width);
    camera.fov = 0.82f;
    camera.near_clip = std::min(camera.near_clip, 0.1f);
    return camera;
}

__attribute__((naked)) void draw_world(void*) {
    // The retail function takes WORLDINFO in ESI, not on the stack.
    asm volatile("push %esi; mov 8(%esp), %esi; mov $0x4927f0, %eax; call *%eax; pop %esi; ret");
}

void set_camera(Camera* camera) {
    at<Camera*>(camera_pointer) = camera;
    fn<void (__cdecl *)(Camera*)>(0x603cc0)(camera);
}

int __cdecl KeepPlayerRoomVisible(void* scene) {
    const int result = fn<int (__cdecl *)(void*)>(0x5eeb30)(scene);
    if (!RenderViewCamera || scene != member<void*>(active_world, 0x13c)) return result;
    const void* object = player(second_view ? 1 : 0);
    const int room = member<int16_t>(object, 0x106a);
    auto* rooms = member<unsigned char*>(scene, 0x78);
    if (!rooms || room < 0 || room >= member<int>(scene, 0x74) || room == member<int>(scene, 0xe0))
        return result;
    auto* player_room = rooms + room * 0x18;
    if ((player_room[0x10] & 4) != 0) return result;
    // An orbit camera can sit across a portal from its player. Keep the
    // occupied room available to this view while the normal frustum culls it.
    fn<void (__cdecl *)(void*, void*)>(0x600be0)(player_room, scene);
    player_room[0x10] |= 4;
    if (!reported_player_room) {
        reported_player_room = true;
        log("Included the player's room when the orbit camera was across a portal.");
    }
    return result;
}

template<class T> struct Com {
    T* p = nullptr;
    ~Com() { if (p) p->Release(); }
    void reset() { if (p) p->Release(); p = nullptr; }
};

struct SplitFrame {
    IDirect3DDevice9* device = nullptr;
    Com<IDirect3DSurface9> back, top, bottom;
    RECT top_rect{}, bottom_rect{};
    float hud_width_fit = 1.0f, hud_height_fit = 1.0f;
    int hud = -1;
    bool captured = false;

    void clear() {
        back.reset();
        top.reset();
        bottom.reset();
        device = nullptr;
        hud = -1;
        captured = false;
    }
};
SplitFrame frame;
Matrix panel_projection[2];
Vec panel_right[2];
int hud_player;

extern "C" int __cdecl CheckSplitForceScreen(const void* object) {
    if (!active_world || active_world != at<const void*>(world_pointer) || !eligible(active_world)) return 0;
    const int slot = object == player(0) ? 0 : object == player(1) ? 1 : -1;
    if (slot < 0) return 0;
    const auto& projection = panel_projection[slot];
    const Vec point = member<Vec>(object, 0x80);
    const float depth = projection.component(point, 3);
    constexpr float edge = 0.85f;
    if (depth <= 0.0f || std::abs(projection.component(point, 0)) > edge * depth) return 2;
    const auto& shared = member<Vec>(object, 0xc5c);
    if (!reported_sustained_force && shared.z > 0.0f && std::abs(shared.x) > edge) {
        reported_sustained_force = true;
        log("Kept Force active for a player outside the old shared view.");
    }
    return 1;
}

extern "C" __attribute__((naked)) void SustainForceEntry() {
    // ESI holds the character and ECX holds the level for the next native check.
    asm volatile("pushal; push %esi; call _CheckSplitForceScreen; add $4, %esp;"
                 "test %eax, %eax; jz 1f; cmp $2, %eax; je 2f;"
                 "popal; jmp *3f;"
                 "1: popal; fldz; fcomps 0xc64(%esi); jmp *4f;"
                 "2: popal; jmp *5f;"
                 "3: .long 0x4a2188; 4: .long 0x4a2153; 5: .long 0x4a2209");
}

bool split_panel() {
    return frame.device && frame.hud >= 0;
}

constexpr float hud_scale = 0.5f;
constexpr float hud_left = -0.84f;
constexpr float hint_scale = 0.5f;
constexpr float shared_total_top = 0.90f;

const RECT& pane_rect(int slot) {
    return slot == 0 ? frame.top_rect : frame.bottom_rect;
}

float pane_screen_y(float y, int slot) {
    const auto& rect = pane_rect(slot);
    const float pixel = float(rect.top) + (1.0f - y) * 0.5f * float(rect.bottom - rect.top);
    return 1.0f - 2.0f * pixel / float(frame.bottom_rect.bottom);
}

float pane_hud_y(float y, int slot) {
    return pane_screen_y(1.0f + (y - 1.0f) * frame.hud_height_fit, slot);
}

void place_player_hud(float& x, float& y, int slot) {
    if (slot == 1) x = -x;
    x = -1.0f + (1.0f + hud_left + (x - hud_left) * hud_scale) * frame.hud_width_fit;
    y = pane_hud_y(y, slot);
}

bool adjust_panel_model;
struct PanelModelAspect {
    bool previous = adjust_panel_model;
    explicit PanelModelAspect(bool enabled) {
        if (enabled) adjust_panel_model = true;
    }
    ~PanelModelAspect() { adjust_panel_model = previous; }
};

void __cdecl TransformPanelModel(Matrix* output, Matrix* model, Matrix* camera) {
    if (adjust_panel_model) {
        const auto& projection = at<Matrix>(0x80b748);
        const float px = projection.m[0][0], py = projection.m[1][1];
        const float native_x = px * at<float>(0x87b938);
        const float native_y = py * at<float>(0x87b998);
        if (native_x > 0.0f && native_y > 0.0f) {
            // Convert model coordinates through the actual native panel camera.
            // The stock widescreen panel's calibration defines the model size.
            constexpr float reference_y = 1145.0f / 387.5f;
            const float scale_y = reference_y / py;
            const float scale_x = reference_y * float(frame.bottom_rect.bottom) / (px * float(frame.top_rect.right));
            for (int row = 0; row < 3; ++row) {
                model->m[row][0] *= scale_x;
                model->m[row][1] *= scale_y;
                model->m[row][2] *= scale_y;
            }
            model->m[3][0] /= native_x;
            model->m[3][1] /= native_y;
        }
    }
    fn<decltype(&TransformPanelModel)>(0x5c0500)(output, model, camera);
}

void scale_panel_text(float scale, float& sx, float& sy, float& sz) {
    sx *= scale * frame.hud_width_fit;
    sy *= scale * frame.hud_height_fit;
    sz *= scale * frame.hud_height_fit;
}

float compact_hint_y(float y) {
    return y < 0.0f ? -1.0f + (y + 0.92f) * frame.hud_height_fit : y;
}

unsigned left_align(unsigned alignment) {
    return alignment & 8 ? (alignment & ~8u) | 2u : alignment;
}

void __cdecl StartPlayerHud(int slot) {
    hud_player = slot;
    fn<decltype(&StartPlayerHud)>(0x6d6340)(slot);
}

void __cdecl PlayerPortrait(int id, float x, float y, float z, float scale, int model,
                            float alpha, float frame_alpha, int visible, void* special) {
    const bool split = split_panel();
    const PanelModelAspect aspect(split);
    if (split) {
        place_player_hud(x, y, hud_player);
        scale *= hud_scale * frame.hud_height_fit;
    }
    fn<decltype(&PlayerPortrait)>(0x55b830)(id, x, y, z, scale, model, alpha, frame_alpha, visible, special);
}

template<bool Hint>
void __cdecl PanelSmartText(char* text, float x, float y, float z, float sx, float sy, float sz,
                           unsigned alignment, unsigned red, unsigned green, unsigned blue, float width,
                           int lines, void* extra, int mode, unsigned alpha) {
    if constexpr (Hint) {
        if (split_panel()) {
            scale_panel_text(hint_scale, sx, sy, sz);
            y = compact_hint_y(y);
            if (!reported_split_hint) {
                reported_split_hint = true;
                log("Drew a compact split-screen gameplay tip.");
            }
        }
    } else if (split_panel()) {
        place_player_hud(x, y, hud_player);
        scale_panel_text(hud_scale, sx, sy, sz);
        width *= hud_scale * frame.hud_width_fit;
        alignment = left_align(alignment);
    }
    fn<decltype(&PanelSmartText<Hint>)>(0x50d910)(
        text, x, y, z, sx, sy, sz, alignment, red, green, blue, width, lines, extra, mode, alpha);
}

void __cdecl PlayerScoreText(char* text, float x, float y, float z, float sx, float sy, float sz,
                             unsigned alignment, unsigned red, unsigned green, unsigned blue, unsigned char alpha) {
    if (split_panel()) {
        place_player_hud(x, y, hud_player);
        scale_panel_text(hud_scale, sx, sy, sz);
        alignment = left_align(alignment);
    }
    fn<decltype(&PlayerScoreText)>(0x50c780)(text, x, y, z, sx, sy, sz, alignment, red, green, blue, alpha);
}

Vec stud_counter_position[2];
bool drew_stud_counter[2];

template<bool StudCounter>
int __cdecl PlayerPanelObject(float x, float y, float z, float sx, float sy, float sz,
                              uint16_t ax, uint16_t ay, uint16_t az, void* special, int mode, float alpha) {
    const bool split = split_panel();
    const PanelModelAspect aspect(split);
    if (split) {
        place_player_hud(x, y, hud_player);
        const float scale = hud_scale * frame.hud_height_fit;
        sx *= scale;
        sy *= scale;
        sz *= scale;
        if constexpr (StudCounter) {
            stud_counter_position[hud_player] = {x, y, z};
            drew_stud_counter[hud_player] = true;
        }
    }
    return fn<decltype(&PlayerPanelObject<StudCounter>)>(0x552c10)(
        x, y, z, sx, sy, sz, ax, ay, az, special, mode, alpha);
}

int __cdecl HintPanelObject(float x, float y, float z, float sx, float sy, float sz,
                            uint16_t ax, uint16_t ay, uint16_t az, void* special, int mode) {
    const bool split = split_panel();
    const PanelModelAspect aspect(split);
    if (split) {
        const float scale = hint_scale * frame.hud_height_fit;
        sx *= scale;
        sy *= scale;
        sz *= scale;
        y = compact_hint_y(y);
    }
    return fn<decltype(&HintPanelObject)>(0x552d10)(x, y, z, sx, sy, sz, ax, ay, az, special, mode);
}

struct HeartArgs {
    float x, y, scale, scale_factor;
    unsigned alignment;
    float alpha;
    int pulse;
};
static_assert(sizeof(HeartArgs) == 28);

extern "C" __attribute__((naked)) void CallNativeHearts(const void*, const HeartArgs*) {
    asm volatile("mov 4(%esp), %eax; mov 8(%esp), %ecx;"
                 "push 24(%ecx); push 20(%ecx); push 16(%ecx); push 12(%ecx);"
                 "push 8(%ecx); push 4(%ecx); push 0(%ecx);"
                 "mov $0x4b88b0, %ecx; call *%ecx; add $28, %esp; ret");
}

extern "C" void __cdecl DrawPlayerHearts(const void* object, const HeartArgs* original) {
    if (!split_panel()) {
        CallNativeHearts(object, original);
        return;
    }
    HeartArgs args = *original;
    place_player_hud(args.x, args.y, hud_player);
    args.scale *= hud_scale * frame.hud_height_fit;
    args.alignment = left_align(args.alignment);
    const PanelModelAspect aspect(true);
    auto& spacing = at<float>(0x7fc308);
    const float stock_spacing = spacing;
    spacing *= frame.hud_width_fit / frame.hud_height_fit;
    CallNativeHearts(object, &args);
    spacing = stock_spacing;
}

extern "C" __attribute__((naked)) void PlayerHeartsEntry() {
    // DrawHitPointsPc takes its object in EAX and its remaining args on the stack.
    asm volatile("lea 4(%esp), %ecx; push %ecx; push %eax; call _DrawPlayerHearts; add $8, %esp; ret");
}

void __cdecl SharedCoinTotal(int total, float y, float scale, int remember, float phase, int r, int g, int b) {
    const auto draw = fn<decltype(&SharedCoinTotal)>(0x4b80e0);
    if (split_panel()) {
        if (y >= 1.0f) return;
        phase *= std::clamp((1.0f - y) / (1.0f - shared_total_top), 0.0f, 1.0f);
        y = std::min(y, shared_total_top);
        const PanelModelAspect aspect(true);
        auto& text_width = at<float>(0x8015ac);
        auto& coin_offset = at<float>(0x7fc36c);
        const float stock_text_width = text_width, stock_coin_offset = coin_offset;
        const float width_ratio = frame.hud_width_fit / frame.hud_height_fit;
        text_width *= width_ratio;
        coin_offset *= width_ratio;
        scale *= hud_scale * frame.hud_height_fit;
        draw(total, pane_hud_y(y, 0), scale, remember, phase, r, g, b);
        draw(total, pane_hud_y(y, 1), scale, remember, phase, r, g, b);
        text_width = stock_text_width;
        coin_offset = stock_coin_offset;
    } else {
        draw(total, y, scale, remember, phase, r, g, b);
    }
}

struct GameMessage {
    unsigned char text_and_origin[0x88];
    Vec target, position, start;
    float scale_start, scale_target, scale_initial, scale_current;
    unsigned char timing[0x1c];
    uint32_t flags;
    unsigned char appearance[0x1c];
    unsigned char active, has_start, expired, behind, text_align, owner;
    unsigned char callbacks[0xa];
    uintptr_t update, draw, end;
};
static_assert(sizeof(GameMessage) == 0x114 && offsetof(GameMessage, owner) == 0xfd
              && offsetof(GameMessage, scale_current) == 0xb8 && offsetof(GameMessage, update) == 0x108);
constexpr uintptr_t game_messages = 0x947bc0;
constexpr size_t game_message_count = 128;

bool stud_message(const GameMessage& message) {
    return message.active && message.owner <= 1 && message.end == 0x46af20 && (message.flags & 1);
}

bool stud_model(const void* special) {
    const uintptr_t address = reinterpret_cast<uintptr_t>(special);
    constexpr uintptr_t first_special = game_messages + 0xe8;
    if (!split_panel() || address < first_special) return false;
    const uintptr_t offset = address - first_special;
    return offset < game_message_count * sizeof(GameMessage) && offset % sizeof(GameMessage) == 0
        && stud_message(at<GameMessage>(game_messages + offset));
}

int __cdecl DrawFlyingStud(float x, float y, float z, float sx, float sy, float sz, uint16_t ax, uint16_t ay,
                          uint16_t az, void* special, int mode, float alpha) {
    const PanelModelAspect aspect(stud_model(special));
    return fn<decltype(&DrawFlyingStud)>(0x552c10)(x, y, z, sx, sy, sz, ax, ay, az, special, mode, alpha);
}

int __cdecl DrawFlyingStudOpaque(float x, float y, float z, float sx, float sy, float sz, uint16_t ax, uint16_t ay,
                                uint16_t az, void* special, int mode) {
    const PanelModelAspect aspect(stud_model(special));
    return fn<decltype(&DrawFlyingStudOpaque)>(0x552d10)(x, y, z, sx, sy, sz, ax, ay, az, special, mode);
}

struct StudDrawState {
    GameMessage* message;
    Vec start, target;
    float scale, target_scale;
    unsigned char behind;
};

Vec project_to_panel(const Matrix& projection, Vec point, int slot) {
    const float depth = std::max(projection.component(point, 3), 0.1f);
    const float x = std::clamp(projection.component(point, 0) / depth, -0.95f, 0.95f);
    const float y = std::clamp(projection.component(point, 1) / depth, -0.92f, 0.92f);
    return {x, pane_screen_y(y, slot), 1.0f};
}

void __cdecl DrawSplitMessages() {
    const auto draw = fn<void (__cdecl *)()>(0x561400);
    if (!split_panel()) {
        draw();
        return;
    }
    auto& messages = at<GameMessage[game_message_count]>(game_messages);
    std::array<StudDrawState, game_message_count> previous;
    size_t count = 0;
    for (auto& message : messages) {
        if (!stud_message(message)) continue;
        auto& target = message.target;
        previous[count++] = {&message, message.start, target, message.scale_current, message.scale_target,
                             message.behind};
        const int slot = message.owner;
        const auto& projection = panel_projection[slot];
        const auto& right = panel_right[slot];
        Vec point = message.position;
        // When a pickup is behind the camera, start the flight at the collector.
        if (projection.component(point, 3) < 0.1f) {
            const void* object = player(slot);
            point = member<Vec>(object, 0x5c);
            point.y += 0.5f * member<float>(object, 0xb4);
        }
        const Vec edge{point.x + message.scale_initial * right.x, point.y + message.scale_initial * right.y,
                       point.z + message.scale_initial * right.z};
        message.start = project_to_panel(projection, point, slot);
        const float depth = std::max(projection.component(edge, 3), 0.1f);
        const float center_depth = std::max(projection.component(point, 3), 0.1f);
        const float projected_size = std::abs(projection.component(edge, 0) / depth
                                            - projection.component(point, 0) / center_depth);
        message.scale_current = std::clamp(projected_size * 0.525f * frame.hud_height_fit / frame.hud_width_fit,
                                           0.02f, 0.16f);
        message.behind = 0;
        message.scale_target *= hud_scale * frame.hud_height_fit;
        if (message.update == 0x46af00) {
            const float shared_y = at<float>(0x7fc380);
            if (shared_y > 0.0f && shared_y < 1.0f) target.y = std::min(shared_y, shared_total_top);
            target.y = pane_hud_y(target.y, slot);
        } else if (drew_stud_counter[slot]) {
            target = stud_counter_position[slot];
        } else {
            place_player_hud(target.x, target.y, slot);
        }
        if (!reported_split_studs[slot]) {
            reported_split_studs[slot] = true;
            log(slot == 0 ? "Projected a stud flight onto Player 1's screen."
                          : "Projected a stud flight onto Player 2's screen.");
        }
    }
    draw();
    // Preserve native state for score callbacks and for a return to solo play.
    for (size_t i = 0; i < count; ++i) {
        const auto& saved = previous[i];
        auto& message = *saved.message;
        message.start = saved.start;
        message.target = saved.target;
        message.scale_current = saved.scale;
        message.scale_target = saved.target_scale;
        message.behind = saved.behind;
    }
}

bool blit(IDirect3DDevice9* device, IDirect3DSurface9* src, IDirect3DSurface9* dst, const RECT* rect) {
    return SUCCEEDED(device->StretchRect(src, nullptr, dst, rect, D3DTEXF_LINEAR));
}

void split_error() {
    if (!reported_error) log("D3D9 could not allocate or copy split view surfaces.");
    reported_error = true;
}

extern "C" void __cdecl BeforeScene(int index) {
    if (!frame.device || flushing_first_view || index != frame.hud) return;
    auto* device = frame.device;
    if (FAILED(device->EndScene())) {
        split_error();
        frame.clear();
        return;
    }
    const bool copied = frame.captured && blit(device, frame.back.p, frame.bottom.p, nullptr)
        && blit(device, frame.top.p, frame.back.p, &frame.top_rect)
        && blit(device, frame.bottom.p, frame.back.p, &frame.bottom_rect);
    frame.clear();
    if (copied && !reported_split) {
        reported_split = true;
        log("Composited two separately submitted world views.");
    }
    const HRESULT restarted = device->BeginScene();
    if (!copied || FAILED(restarted)) split_error();
}

extern "C" __attribute__((naked)) void SceneEntry() {
    asm volatile("pushfl; pushal; push %ebp; call _BeforeScene; add $4, %esp; popal; popfl;"
                 "fld1; mov -0x14(%esi), %edx; ret");
}

void __cdecl AfterScenes() {
    if (frame.hud >= 0) BeforeScene(frame.hud);
    fn<void (__cdecl *)()>(0x5decd0)();
}

D3DPRESENT_PARAMETERS& present_parameters() {
    return at<D3DPRESENT_PARAMETERS>(graphics_backend + 0x138);
}

bool keyboard_allows_windows_key;
bool reported_keyboard_error;
constexpr DWORD stock_keyboard_cooperative = DISCL_FOREGROUND | DISCL_NONEXCLUSIVE | DISCL_NOWINKEY;

HRESULT WINAPI ConfigureGameKeyboard(IDirectInputDevice8A* keyboard, HWND window, DWORD flags) {
    if (present_parameters().Windowed) flags &= ~DISCL_NOWINKEY;
    const HRESULT result = keyboard->SetCooperativeLevel(window, flags);
    if (SUCCEEDED(result)) keyboard_allows_windows_key = !(flags & DISCL_NOWINKEY);
    return result;
}

void sync_keyboard_cooperative() {
    const bool windowed = present_parameters().Windowed != FALSE;
    if (windowed == keyboard_allows_windows_key) return;
    auto* keyboard = at<IDirectInputDevice8A*>(game_keyboard);
    const HWND window = at<HWND>(game_window);
    if (!keyboard || !window) return;

    // DirectInput changes the keyboard policy while the device is unacquired.
    keyboard->Unacquire();
    const HRESULT result = ConfigureGameKeyboard(keyboard, window, stock_keyboard_cooperative);
    keyboard->Acquire();
    if (SUCCEEDED(result)) {
        reported_keyboard_error = false;
        log(windowed ? "Windows key enabled in windowed display mode."
                     : "Restored the stock fullscreen keyboard policy.");
    } else if (!reported_keyboard_error) {
        reported_keyboard_error = true;
        log("Could not update the Windows key policy.");
    }
}

void __cdecl EndFrameInput() {
    if (flushing_first_view) return;
    sync_keyboard_cooperative();
    fn<void (__cdecl *)()>(0x6d9610)();
}

int __cdecl PresentOrCapture(int mode, int flags) {
    if (!flushing_first_view) return fn<int (__cdecl *)(int, int)>(0x6dfd40)(mode, flags);
    frame.captured = blit(frame.device, frame.back.p, frame.top.p, nullptr);
    return 0;
}

void flush_first_view() {
    // The engine shares its static display lists between cameras. Finish the
    // first view and recycle those lists before submitting the second view.
    auto& wind_tick = at<uint32_t>(0x97390c);
    auto& shader_time = at<float>(0x2857274);
    auto& frame_callback = at<void*>(0x2854ce4);
    const uint32_t saved_wind_tick = wind_tick;
    const float saved_shader_time = shader_time;
    void* saved_frame_callback = frame_callback;
    frame_callback = nullptr;
    flushing_first_view = true;
    fn<int (__cdecl *)(int)>(0x60aff0)(1);
    flushing_first_view = false;
    frame_callback = saved_frame_callback;
    shader_time = saved_shader_time;
    wind_tick = saved_wind_tick;
    // SwapScreen queues an empty callback scene for the next frame.
    if (at<int>(scene_count) == 1 && at<int>(scene_array) == -1)
        at<int>(scene_count) = 0;
}

void disable_full_frame_effects(int start, int end) {
    for (int i = start; i < end; ++i) {
        const uintptr_t scene = scene_array + i * 0x218;
        for (auto field : {0x48, 0x58, 0xac, 0x188}) at<uint32_t>(scene + field) = 0;
    }
}

extern "C" void __cdecl DrawSplit(void* world) {
    frame.clear();
    drew_stud_counter[0] = drew_stud_counter[1] = false;
    first_character_objects = nullptr;
    first_parts = nullptr;
    if (stranger_customizer_open()) {
        // The hub supplies the stock shared camera. Keep the orbit state for exit.
        draw_world(world);
        return;
    }
    const int first_scene = at<int>(scene_count);
    // The game can queue a clear before it starts drawing the world.
    if (!eligible(world) || first_scene < 0 || first_scene > 2) {
        active_world = nullptr;
        draw_world(world);
        return;
    }
    auto* device = fn<IDirect3DDevice9* (__cdecl *)()>(0x6e4570)();
    D3DSURFACE_DESC desc{};
    if (!device || FAILED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &frame.back.p))
        || FAILED(frame.back.p->GetDesc(&desc)) || desc.Width == 0 || desc.Height < 2
        || desc.MultiSampleType != D3DMULTISAMPLE_NONE) {
        if (device) split_error();
        frame.clear();
        active_world = nullptr;
        draw_world(world);
        return;
    }
    const LONG width = static_cast<LONG>(desc.Width), height = static_cast<LONG>(desc.Height);
    const LONG midpoint = height / 2;
    frame.top_rect = {0, 0, width, midpoint};
    frame.bottom_rect = {0, midpoint, width, height};
    constexpr float reference_aspect = 16.0f / 9.0f;
    const float relative_aspect = float(width) / (float(height) * reference_aspect);
    frame.hud_width_fit = std::min(1.0f, 1.0f / relative_aspect);
    frame.hud_height_fit = std::min(1.0f, relative_aspect);
    if (FAILED(device->CreateRenderTarget(width, midpoint, desc.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &frame.top.p, nullptr))
        || FAILED(device->CreateRenderTarget(width, height - midpoint, desc.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &frame.bottom.p, nullptr))) {
        split_error();
        frame.clear();
        active_world = nullptr;
        draw_world(world);
        return;
    }
    const bool entering = active_world != world;
    active_world = world;
    Camera* original = at<Camera*>(camera_pointer);
    update_orbit(0, entering);
    update_orbit(1, entering);
    Camera camera1 = make_view(*original, 0, desc, midpoint);
    Camera camera2 = make_view(*original, 1, desc, height - midpoint);
    frame.device = device;
    set_camera(&camera1);
    panel_projection[0] = at<Matrix>(0x80b888);
    panel_right[0] = {camera1.matrix.m[0][0], camera1.matrix.m[0][1], camera1.matrix.m[0][2]};
    force_views[0] = {at<Matrix>(0x80b708), at<Matrix>(0x80b988), camera1.near_clip, camera1.far_clip};
    RenderViewCamera = &camera1;
    draw_world(world);
    RenderViewCamera = nullptr;
    const WorldVisibility first_visibility(world);
    capture_pickups(member<const PickupRuntime*>(world, 0x50b8));
    disable_full_frame_effects(first_scene, at<int>(scene_count));
    flush_first_view();
    if (!frame.captured) {
        split_error();
        frame.clear();
        set_camera(original);
        active_world = nullptr;
        draw_world(world);
        return;
    }
    const int second_start = at<int>(scene_count);
    second_view = true;
    set_camera(&camera2);
    panel_projection[1] = at<Matrix>(0x80b888);
    panel_right[1] = {camera2.matrix.m[0][0], camera2.matrix.m[0][1], camera2.matrix.m[0][2]};
    force_views[1] = {at<Matrix>(0x80b708), at<Matrix>(0x80b988), camera2.near_clip, camera2.far_clip};
    RenderViewCamera = &camera2;
    draw_world(world);
    RenderViewCamera = nullptr;
    // The next gameplay update needs to know what's visible to either player.
    first_visibility.merge_into(world);
    merge_pickups(member<const PickupRuntime*>(world, 0x50b8));
    second_view = false;
    set_camera(original);
    const int end = at<int>(scene_count);
    disable_full_frame_effects(second_start, end);
    frame.hud = end;
}

extern "C" __attribute__((naked)) void RenderEntry() {
    asm volatile("push %esi; call _DrawSplit; add $4, %esp; ret");
}

uint16_t __cdecl PlayerAngle(const void* object, const void* pad) {
    if (active_world && eligible(active_world) && pad) {
        for (int slot = 0; slot < 2; ++slot) {
            if (object != player(slot)) continue;
            // Ordinary rooms also have camera sockets. Only sockets whose
            // input-lock flag is set should keep the stock movement basis.
            const int socket_index = member<signed char>(object, 0x671);
            if (socket_index >= 0) {
                const void* socket_system = member<const void*>(active_world, 0x2988);
                const auto* sockets = socket_system ? member<const unsigned char*>(socket_system, 0) : nullptr;
                if (sockets && (member<uint32_t>(sockets + socket_index * 0x13c, 0x68) & 0x40))
                    break;
            }
            return uint16_t(member<uint16_t>(pad, 0x26) + int(orbit[slot].yaw * (65536.0f / tau)));
        }
    }
    return original_angle(object, pad);
}

void __cdecl UpdateBlur() { if (!second_view) fn<void (__cdecl *)()>(0x42df70)(); }
void __cdecl UpdateShadows() { if (!second_view) fn<void (__cdecl *)()>(0x589f80)(); }
void __cdecl UpdateCharacters() {
    if (RenderViewCamera && !second_view) {
        capture_character_draw();
        return;
    }
    if (second_view) merge_character_draw();
    fn<void (__cdecl *)()>(0x4b1ab0)();
}
void __cdecl KeepPlayerOnScreen(void* object) {
    if ((object == player(0) || object == player(1)) && local_free_roam(at<void*>(world_pointer))) {
        at<float>(reinterpret_cast<uintptr_t>(object) + 0xda8) = 0.0f;
        return;
    }
    fn<void (__cdecl *)(void*)>(0x415770)(object);
}

extern "C" int __cdecl SkipAutomaticDropout() {
    if (!both_players_joined()) return 0;
    if (!reported_auto_dropout) {
        reported_auto_dropout = true;
        log("Kept both players active after the stock off-screen check.");
    }
    return 1;
}

extern "C" __attribute__((naked)) void AutomaticDropoutEntry() {
    // This block belongs only to the stock off-screen check. The pause
    // menu and other callers of DropOut keep their original behavior.
    asm volatile("pushfl; pushal; call _SkipAutomaticDropout; test %eax, %eax; popal; jz 1f;"
                 "popfl; mov $0x428924, %eax; jmp *%eax;"
                 "1: popfl; flds 0x7f6734; mov $0x4288ff, %eax; jmp *%eax");
}

void make_branch(unsigned char* dest, uintptr_t from, uintptr_t to, unsigned char opcode) {
    dest[0] = opcode;
    const uint32_t displacement = uint32_t(to - from - 5);
    std::memcpy(dest + 1, &displacement, sizeof(displacement));
}

bool patch_branch(uintptr_t from, uintptr_t to, unsigned char opcode) {
    auto* dest = reinterpret_cast<unsigned char*>(from);
    DWORD old;
    if (!VirtualProtect(dest, 5, PAGE_EXECUTE_READWRITE, &old)) return false;
    make_branch(dest, from, to, opcode);
    FlushInstructionCache(GetCurrentProcess(), dest, 5);
    VirtualProtect(dest, 5, old, &old);
    return true;
}

bool call_matches(uintptr_t from, uintptr_t to) {
    unsigned char expected[5];
    make_branch(expected, from, to, 0xe8);
    return std::memcmp(reinterpret_cast<void*>(from), expected, 5) == 0;
}

struct CallHook { uintptr_t site, original, replacement; };

enum class DisplayMode { Fullscreen, Windowed, Borderless };
constexpr uintptr_t video_options = 0x921d04;
constexpr uintptr_t menu_sound = 0x800940;
constexpr int display_mode_row = 4;
constexpr int video_apply_row = 5;
using MenuFn = void (__cdecl *)(void*);
using DrawMenuRow = void (__cdecl *)(void*, char*, int);

struct DisplaySnapshot {
    DisplayMode mode;
    D3DPRESENT_PARAMETERS parameters;
    RECT bounds;
    LONG style, extended_style;
    UINT resolution;
    std::array<int, 5> options;
};

struct DisplayReset {
    DisplaySnapshot previous;
    DisplayMode target;
    HMONITOR monitor;
    D3DDISPLAYMODE desktop;
    const DisplaySnapshot* restore;
    HRESULT result = E_UNEXPECTED;
};

DisplayMode display_selection;
D3DDISPLAYMODE video_menu_desktop;
bool borderless_active;
DisplayReset* display_reset;
bool display_confirmation;
DisplaySnapshot display_before_confirmation;
RECT last_windowed_bounds;
bool have_windowed_bounds;

DisplayMode current_display_mode() {
    if (!present_parameters().Windowed) return DisplayMode::Fullscreen;
    return borderless_active ? DisplayMode::Borderless : DisplayMode::Windowed;
}

DisplaySnapshot capture_display() {
    HWND window = at<HWND>(game_window);
    DisplaySnapshot snapshot{};
    snapshot.mode = current_display_mode();
    snapshot.parameters = present_parameters();
    GetWindowRect(window, &snapshot.bounds);
    snapshot.style = GetWindowLongW(window, GWL_STYLE);
    snapshot.extended_style = GetWindowLongW(window, GWL_EXSTYLE);
    snapshot.resolution = at<UINT>(graphics_backend + 0x620);
    snapshot.options = {at<int>(0x826494), at<uint8_t>(0x8264c4) != 0,
                        at<uint8_t>(0x8264b8) != 0, at<uint8_t>(0x8264c6) != 0,
                        at<uint8_t>(0x8264c8) != 0};
    return snapshot;
}

void apply_video_options() {
    fn<void (__thiscall *)(void*)>(0x509230)(reinterpret_cast<void*>(video_options));
}

bool read_monitor(HMONITOR monitor, MONITORINFOEXW& info) {
    info.cbSize = sizeof(info);
    return GetMonitorInfoW(monitor, &info) != FALSE;
}

D3DDISPLAYMODE desktop_mode(HMONITOR monitor) {
    MONITORINFOEXW info{};
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    const DWORD settings = present_parameters().Windowed ? ENUM_CURRENT_SETTINGS : ENUM_REGISTRY_SETTINGS;
    if (read_monitor(monitor, info) && EnumDisplaySettingsW(info.szDevice, settings, &mode)
        && mode.dmPelsWidth && mode.dmPelsHeight) {
        return {mode.dmPelsWidth, mode.dmPelsHeight, mode.dmDisplayFrequency, D3DFMT_UNKNOWN};
    }
    return at<D3DDISPLAYMODE>(graphics_backend + 0x608);
}

HMONITOR display_monitor(HWND window, DisplayMode target) {
    // The existing D3D9 device can enter exclusive mode only on its own adapter.
    if (target == DisplayMode::Fullscreen) {
        auto* device = at<IDirect3DDevice9*>(graphics_backend + 4);
        auto* factory = at<IDirect3D9*>(graphics_backend);
        D3DDEVICE_CREATION_PARAMETERS creation{};
        if (device && factory && SUCCEEDED(device->GetCreationParameters(&creation))) {
            if (const auto monitor = factory->GetAdapterMonitor(creation.AdapterOrdinal)) return monitor;
        }
    }
    return MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
}

bool position_window(HWND window, LONG style, LONG extended_style, const RECT& bounds) {
    SetLastError(0);
    if (!SetWindowLongW(window, GWL_STYLE, style) && GetLastError()) return false;
    SetLastError(0);
    if (!SetWindowLongW(window, GWL_EXSTYLE, extended_style) && GetLastError()) return false;
    const HWND order = extended_style & WS_EX_TOPMOST ? HWND_TOPMOST : HWND_NOTOPMOST;
    return SetWindowPos(window, order, bounds.left, bounds.top, bounds.right - bounds.left,
                        bounds.bottom - bounds.top, SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_FRAMECHANGED) != FALSE;
}

bool position_display(const DisplayReset& reset, const D3DPRESENT_PARAMETERS& parameters) {
    const HWND window = parameters.hDeviceWindow;
    if (reset.restore) {
        const auto& saved = *reset.restore;
        return position_window(window, saved.style, saved.extended_style, saved.bounds);
    }
    MONITORINFOEXW info{};
    if (!read_monitor(reset.monitor, info)) return false;
    const LONG common_style = reset.previous.style & (WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS);
    const LONG normal_ex = reset.previous.extended_style & ~(WS_EX_TOPMOST | WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE);
    if (reset.target == DisplayMode::Borderless) {
        return position_window(window, common_style | WS_POPUP, normal_ex, info.rcMonitor);
    }
    if (reset.target == DisplayMode::Fullscreen) {
        return position_window(window, common_style | WS_POPUP, GetWindowLongW(window, GWL_EXSTYLE),
                               info.rcMonitor);
    }
    const LONG style = common_style | WS_OVERLAPPEDWINDOW;
    const RECT work = info.rcWork;
    RECT bounds = last_windowed_bounds;
    if (!have_windowed_bounds || bounds.left < work.left || bounds.top < work.top
        || bounds.right > work.right || bounds.bottom > work.bottom) {
        const LONG work_width = work.right - work.left, work_height = work.bottom - work.top;
        const double scale = std::min({1.0, 0.8 * work_width / parameters.BackBufferWidth,
                                      0.8 * work_height / parameters.BackBufferHeight});
        bounds = {0, 0, LONG(parameters.BackBufferWidth * scale), LONG(parameters.BackBufferHeight * scale)};
        AdjustWindowRectEx(&bounds, style, FALSE, normal_ex);
        const LONG width = bounds.right - bounds.left, height = bounds.bottom - bounds.top;
        bounds.left = work.left + (work_width - width) / 2;
        bounds.top = work.top + (work_height - height) / 2;
        bounds.right = bounds.left + width;
        bounds.bottom = bounds.top + height;
    }
    return position_window(window, style, normal_ex, bounds);
}

void sync_render_size(const D3DPRESENT_PARAMETERS& parameters) {
    at<UINT>(0x973900) = parameters.BackBufferWidth;
    at<UINT>(0x973904) = parameters.BackBufferHeight;
    at<UINT>(0x80202c) = parameters.BackBufferWidth / 2;
    at<UINT>(0x802030) = parameters.BackBufferHeight / 2;
}

// The native renderer has released its default-pool resources at both Reset sites.
extern "C" HRESULT WINAPI ResetDisplayDevice(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* parameters) {
    if (!display_reset) return device->Reset(parameters);
    auto& reset = *display_reset;
    D3DPRESENT_PARAMETERS requested = reset.restore ? reset.restore->parameters : *parameters;
    if (!reset.restore && reset.target == DisplayMode::Borderless) {
        requested.BackBufferWidth = reset.desktop.Width;
        requested.BackBufferHeight = reset.desktop.Height;
        requested.FullScreen_RefreshRateInHz = 0;
    }
    D3DPRESENT_PARAMETERS argument = requested;
    reset.result = device->Reset(&argument);
    if (SUCCEEDED(reset.result) && position_display(reset, requested)) {
        *parameters = requested;
        sync_render_size(requested);
        return reset.result;
    }
    if (SUCCEEDED(reset.result)) reset.result = E_FAIL;

    const auto& saved = reset.previous;
    at<UINT>(graphics_backend + 0x660) = saved.resolution;
    at<uint8_t>(graphics_backend + 0x65c) = saved.parameters.Windowed != FALSE;
    *parameters = saved.parameters;
    argument = saved.parameters;
    const HRESULT fallback = device->Reset(&argument);
    position_window(saved.parameters.hDeviceWindow, saved.style, saved.extended_style, saved.bounds);
    if (SUCCEEDED(fallback)) sync_render_size(saved.parameters);
    log(SUCCEEDED(fallback) ? "Display change failed. Restored the previous mode."
                            : "Direct3D display reset deferred to device recovery.");
    return fallback;
}

extern "C" __attribute__((naked)) void DisplayResetEntry() {
    // On double failure skip rebuilding surfaces on a lost device. Release the
    // renderer lock through its normal epilogue so Present can retry recovery.
    asm volatile("push 8(%esp); push 8(%esp); call _ResetDisplayDevice@8;"
                 "test %eax, %eax; jns 1f; movl $0x6e4159, (%esp); 1: ret $8");
}

UINT __fastcall FindDisplayResolution(void* backend, void*, UINT width, UINT height, UINT refresh) {
    if (display_reset) return at<UINT>(graphics_backend + 0x660);
    return fn<UINT (__thiscall *)(void*, UINT, UINT, UINT)>(0x6e1b70)(backend, width, height, refresh);
}

using NativeReset = void (__thiscall *)(void*, int);

bool switch_display(DisplayMode target, const DisplaySnapshot& previous, const DisplaySnapshot* restore) {
    const HWND window = at<HWND>(game_window);
    const auto monitor = display_monitor(window, target);
    if (!monitor) return false;
    DisplayReset reset{previous, target, monitor, desktop_mode(monitor), restore};
    at<uint8_t>(graphics_backend + 0x65c) = target != DisplayMode::Fullscreen;
    if (restore) at<UINT>(graphics_backend + 0x660) = restore->resolution;
    frame.clear();
    display_reset = &reset;
    fn<NativeReset>(0x6e3940)(reinterpret_cast<void*>(graphics_backend), 0);
    display_reset = nullptr;
    if (FAILED(reset.result)) {
        present_parameters() = previous.parameters;
        at<uint8_t>(graphics_backend + 0x65c) = previous.parameters.Windowed != FALSE;
        at<UINT>(graphics_backend + 0x660) = previous.resolution;
        return false;
    }
    borderless_active = target == DisplayMode::Borderless;
    return true;
}

void __fastcall ResetActiveDisplay(void* backend, void*, int reposition) {
    frame.clear();
    const DisplayMode mode = current_display_mode();
    if (mode != DisplayMode::Borderless) {
        fn<NativeReset>(0x6e3940)(backend, mode == DisplayMode::Windowed ? 0 : reposition);
        return;
    }
    switch_display(DisplayMode::Borderless, capture_display(), nullptr);
}

void __cdecl EnterDisplayMenu(void* menu) {
    fn<MenuFn>(0x5092c0)(menu);
    display_selection = current_display_mode();
    video_menu_desktop = desktop_mode(MonitorFromWindow(at<HWND>(game_window), MONITOR_DEFAULTTONEAREST));
}

void __cdecl DrawDisplayModeRow(void* menu, char* text, int alpha) {
    constexpr const char* modes[] = {"Fullscreen", "Windowed", "Borderless"};
    char row[64];
    std::snprintf(row, sizeof(row), "Display Mode: %s", modes[static_cast<int>(display_selection)]);
    const auto draw = fn<DrawMenuRow>(0x50f740);
    draw(menu, row, at<uint8_t>(0x921d60));
    draw(menu, text, alpha);
}

bool can_change_display() {
    auto* device = at<IDirect3DDevice9*>(graphics_backend + 4);
    return at<int>(0x827610) != 0 && device && device->TestCooperativeLevel() == D3D_OK;
}

template<bool Refresh> void __cdecl DrawDisplayDimension(void* menu, char* text, int alpha) {
    const bool desktop_value = display_selection == DisplayMode::Borderless
        || (Refresh && display_selection == DisplayMode::Windowed);
    if (!desktop_value) {
        fn<DrawMenuRow>(0x50f740)(menu, text, alpha);
        return;
    }
    const auto desktop = video_menu_desktop;
    char row[96];
    if constexpr (Refresh) {
        std::snprintf(row, sizeof(row), "%s: %u Hz (Desktop)", at<const char*>(0x297add0), desktop.RefreshRate);
    } else {
        std::snprintf(row, sizeof(row), "%s: %u x %u (Desktop)", at<const char*>(0x297adcc),
                      desktop.Width, desktop.Height);
    }
    fn<DrawMenuRow>(0x50f740)(menu, row, alpha / 2);
}

uint8_t __fastcall DisplaySelectionChanged(void* backend, void*) {
    return display_selection != current_display_mode()
        || fn<uint8_t (__thiscall *)(void*)>(0x6e2b40)(backend);
}

void __cdecl UpdateDisplayMenu(void* menu) {
    const auto stock_update = fn<MenuFn>(0x50ef80);
    const auto row = member<int16_t>(menu, 0x0a);
    const bool confirm = member<int>(menu, 0x64) != 0;
    if (member<int>(menu, 0x68)) {
        stock_update(menu);
    } else if (row == display_mode_row) {
        const int step = member<int>(menu, 0x5c) ? 2 : (member<int>(menu, 0x60) || confirm ? 1 : 0);
        if (step) {
            display_selection = static_cast<DisplayMode>((static_cast<int>(display_selection) + step) % 3);
            at<int>(menu_sound) = at<int>(0x800950);
        }
    } else if (row == video_apply_row && confirm) {
        const DisplaySnapshot previous = capture_display();
        if (display_selection == previous.mode) {
            fn<MenuFn>(0x50ee70)(menu);
            return;
        }
        if (!can_change_display()) return;
        if (previous.mode == DisplayMode::Windowed && !(previous.style & (WS_MAXIMIZE | WS_MINIMIZE))) {
            last_windowed_bounds = previous.bounds;
            have_windowed_bounds = true;
        }
        apply_video_options();
        if (switch_display(display_selection, previous, nullptr)) {
            display_before_confirmation = previous;
            display_confirmation = true;
            at<int>(menu_sound) = at<int>(0x800954);
            fn<void (__cdecl *)()>(0x509ab0)();
            fn<void (__cdecl *)(int, int, int)>(0x509830)(1027, 0, -1);
        } else {
            at<std::array<int, 5>>(video_options) = previous.options;
            apply_video_options();
            display_selection = previous.mode;
            at<int>(menu_sound) = at<int>(0x80095c);
        }
    } else if ((row == 0 && display_selection != DisplayMode::Borderless)
               || (row == 1 && display_selection == DisplayMode::Fullscreen) || row == 2 || row == 3) {
        stock_update(menu);
    }
}

void __cdecl UpdateDisplayConfirmation(void* menu) {
    if (!display_confirmation || at<int>(0x921d1c)) {
        fn<MenuFn>(0x50f260)(menu);
        return;
    }
    const bool confirmed = member<int>(menu, 0x64) != 0;
    const bool cancel = member<int>(menu, 0x68) != 0;
    const bool expired = member<float>(menu, 0) >= at<float>(0x921d18);
    if (!cancel && !expired && !confirmed) return;
    if (!cancel && !expired && member<int16_t>(menu, 0x0a) == 0) {
        display_confirmation = false;
        fn<MenuFn>(0x50f260)(menu);
        return;
    }
    if (!can_change_display()) return;
    const auto previous = capture_display();
    const auto& restore = display_before_confirmation;
    at<std::array<int, 5>>(video_options) = restore.options;
    apply_video_options();
    if (!switch_display(restore.mode, previous, &restore)) {
        at<std::array<int, 5>>(video_options) = previous.options;
        apply_video_options();
    }
    display_selection = current_display_mode();
    display_confirmation = false;
    at<int>(menu_sound) = at<int>(0x800958);
    fn<void (__cdecl *)()>(0x509ab0)();
}

const CallHook display_calls[] = {
    {0x511f05, 0x50f740, reinterpret_cast<uintptr_t>(DrawDisplayDimension<false>)},
    {0x511f43, 0x50f740, reinterpret_cast<uintptr_t>(DrawDisplayDimension<true>)},
    {0x5120eb, 0x6e2b40, reinterpret_cast<uintptr_t>(DisplaySelectionChanged)},
    {0x5121a1, 0x50f740, reinterpret_cast<uintptr_t>(DrawDisplayModeRow)},
    {0x50ee99, 0x6e3940, reinterpret_cast<uintptr_t>(ResetActiveDisplay)},
    {0x6e41e3, 0x6e3940, reinterpret_cast<uintptr_t>(ResetActiveDisplay)},
    {0x6e42bb, 0x6e3940, reinterpret_cast<uintptr_t>(ResetActiveDisplay)},
    {0x6e3fc7, 0x6e1b70, reinterpret_cast<uintptr_t>(FindDisplayResolution)},
};
const CallHook display_callbacks[] = {
    {0x800d68, 0x5092c0, reinterpret_cast<uintptr_t>(EnterDisplayMenu)},
    {0x800d70, 0x50ef80, reinterpret_cast<uintptr_t>(UpdateDisplayMenu)},
    {0x800da8, 0x50f260, reinterpret_cast<uintptr_t>(UpdateDisplayConfirmation)},
};
constexpr uintptr_t display_reset_calls[] = {0x6e3e80, 0x6e3ed0};
constexpr unsigned char stock_reset_call[] = {0x8b, 0x42, 0x40, 0xff, 0xd0};

bool enable_display_menu() {
    for (const auto& hook : display_calls) {
        if (!call_matches(hook.site, hook.original)) return false;
    }
    for (const auto& hook : display_callbacks) {
        if (at<uintptr_t>(hook.site) != hook.original) return false;
    }
    for (const auto site : display_reset_calls) {
        if (std::memcmp(reinterpret_cast<const void*>(site), stock_reset_call, 5)) return false;
    }
    auto* table = reinterpret_cast<void*>(display_callbacks[0].site);
    constexpr size_t table_size = 0x44;
    DWORD table_protection;
    if (!VirtualProtect(table, table_size, PAGE_READWRITE, &table_protection)) return false;
    size_t applied_calls = 0, applied_resets = 0;
    for (const auto& hook : display_calls) {
        if (!patch_branch(hook.site, hook.replacement, 0xe8)) break;
        ++applied_calls;
    }
    if (applied_calls == std::size(display_calls)) {
        for (const auto site : display_reset_calls) {
            if (!patch_branch(site, reinterpret_cast<uintptr_t>(DisplayResetEntry), 0xe8)) break;
            ++applied_resets;
        }
    }
    const bool enabled = applied_resets == std::size(display_reset_calls);
    if (enabled) {
        for (const auto& hook : display_callbacks) at<uintptr_t>(hook.site) = hook.replacement;
    } else {
        for (size_t i = 0; i < applied_calls; ++i) {
            const auto& hook = display_calls[i];
            patch_branch(hook.site, hook.original, 0xe8);
        }
        for (size_t i = 0; i < applied_resets; ++i) {
            auto* site = reinterpret_cast<void*>(display_reset_calls[i]);
            DWORD old;
            if (VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &old)) {
                std::memcpy(site, stock_reset_call, 5);
                FlushInstructionCache(GetCurrentProcess(), site, 5);
                VirtualProtect(site, 5, old, &old);
            }
        }
    }
    VirtualProtect(table, table_size, table_protection, &table_protection);
    return enabled;
}

const CallHook presentation_hooks[] = {
    {0x435fdb, 0x4876a0, reinterpret_cast<uintptr_t>(TagPartyCharacter)},
    {0x487d46, 0x4876a0, reinterpret_cast<uintptr_t>(TagPartyCharacter)},
    {0x4880e2, 0x4876a0, reinterpret_cast<uintptr_t>(TagPartyCharacter)},
    {0x4881a6, 0x4876a0, reinterpret_cast<uintptr_t>(TagPartyCharacter)},
    {0x452798, 0x534e10, reinterpret_cast<uintptr_t>(FollowPartyOwner)},
    {0x552ce8, 0x5c0500, reinterpret_cast<uintptr_t>(TransformPanelModel)},
    {0x552ddb, 0x5c0500, reinterpret_cast<uintptr_t>(TransformPanelModel)},
    {0x552e30, 0x5c0500, reinterpret_cast<uintptr_t>(TransformPanelModel)},
    {0x5616dd, 0x552d10, reinterpret_cast<uintptr_t>(DrawFlyingStudOpaque)},
    {0x56174c, 0x552c10, reinterpret_cast<uintptr_t>(DrawFlyingStud)},
    {0x55f67d, 0x5eeb30, reinterpret_cast<uintptr_t>(KeepPlayerRoomVisible)},
    {0x5f3d0c, 0x5eeb30, reinterpret_cast<uintptr_t>(KeepPlayerRoomVisible)},
    {0x4b9261, 0x6d6340, reinterpret_cast<uintptr_t>(StartPlayerHud)},
    {0x4b9479, 0x55b830, reinterpret_cast<uintptr_t>(PlayerPortrait)},
    {0x4b95b7, 0x50d910, reinterpret_cast<uintptr_t>(PanelSmartText<false>)},
    {0x4b967e, 0x50d910, reinterpret_cast<uintptr_t>(PanelSmartText<false>)},
    {0x4b99e5, 0x50d910, reinterpret_cast<uintptr_t>(PanelSmartText<false>)},
    {0x4b9924, 0x4b88b0, reinterpret_cast<uintptr_t>(PlayerHeartsEntry)},
    {0x4b98b5, 0x50c780, reinterpret_cast<uintptr_t>(PlayerScoreText)},
    {0x4b9ac5, 0x50c780, reinterpret_cast<uintptr_t>(PlayerScoreText)},
    {0x4b9baa, 0x50c780, reinterpret_cast<uintptr_t>(PlayerScoreText)},
    {0x4b9793, 0x552c10, reinterpret_cast<uintptr_t>(PlayerPanelObject<false>)},
    {0x4b9807, 0x552c10, reinterpret_cast<uintptr_t>(PlayerPanelObject<false>)},
    {0x4b9c57, 0x552c10, reinterpret_cast<uintptr_t>(PlayerPanelObject<true>)},
    {0x4b86c1, 0x4b80e0, reinterpret_cast<uintptr_t>(SharedCoinTotal)},
    {0x4bba52, 0x561400, reinterpret_cast<uintptr_t>(DrawSplitMessages)},
    {0x4581d6, 0x50d910, reinterpret_cast<uintptr_t>(PanelSmartText<true>)},
    {0x45827e, 0x50d910, reinterpret_cast<uintptr_t>(PanelSmartText<true>)},
    {0x458359, 0x552d10, reinterpret_cast<uintptr_t>(HintPanelObject)},
    {0x4583fa, 0x552d10, reinterpret_cast<uintptr_t>(HintPanelObject)},
};
}

extern "C" __declspec(dllexport) DWORD WINAPI EnableSplit(void*) {
    if (reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)) != 0x400000
        || std::memcmp(reinterpret_cast<void*>(render_call), render_bytes, sizeof(render_bytes))
        || std::memcmp(reinterpret_cast<void*>(input_angle), angle_bytes, sizeof(angle_bytes))
        || std::memcmp(reinterpret_cast<void*>(scene_loop), scene_bytes, sizeof(scene_bytes))
        || std::memcmp(reinterpret_cast<void*>(automatic_dropout), dropout_bytes, sizeof(dropout_bytes))
        || std::memcmp(reinterpret_cast<void*>(pickup_camera_load), pickup_camera_bytes, sizeof(pickup_camera_bytes))
        || std::memcmp(reinterpret_cast<void*>(sustain_force_clip), sustain_force_bytes, sizeof(sustain_force_bytes))
        || std::memcmp(reinterpret_cast<void*>(offscreen_bolt), offscreen_bolt_bytes, sizeof(offscreen_bolt_bytes))
        || std::memcmp(reinterpret_cast<void*>(player_trigger_node), player_node_bytes, sizeof(player_node_bytes))
        || std::memcmp(reinterpret_cast<void*>(player_trigger_area), player_trigger_bytes, sizeof(player_trigger_bytes))
        || std::memcmp(reinterpret_cast<void*>(keyboard_cooperative_call), keyboard_cooperative_bytes,
                       sizeof(keyboard_cooperative_bytes))
        || !call_matches(0x492832, 0x42df70) || !call_matches(0x4928d2, 0x589f80)
        || !call_matches(0x4928d7, 0x4b1ab0) || !call_matches(0x60b513, 0x5decd0)
        || !call_matches(0x60b56e, 0x6d9610) || !call_matches(0x60b57a, 0x6dfd40)
        || !call_matches(0x43590a, 0x415770) || !call_matches(0x4b6f1a, 0x415770)
        || !call_matches(0x49257d, 0x55d1e0) || !call_matches(force_target_clip, 0x5d6620)
        || !call_matches(0x41ecc2, 0x4a4ce0) || !call_matches(0x41ed49, 0x4a4ce0)
        || !call_matches(0x41f5ed, 0x4a4ce0) || !call_matches(0x4a4ebf, 0x5b5700)
        || !call_matches(0x4a4f95, 0x5b5700) || !call_matches(0x492882, 0x64f5d0)
        || !call_matches(0x64f751, 0x64ac90)) {
        log("Unsupported or already modified game code.");
        return 0;
    }
    for (const auto& hook : presentation_hooks) {
        if (!call_matches(hook.site, hook.original)) {
            log("Unsupported or already modified HUD or room renderer code.");
            return 0;
        }
    }
    auto* trampoline = static_cast<unsigned char*>(VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!trampoline) return 0;
    std::memcpy(trampoline, angle_bytes, sizeof(angle_bytes));
    make_branch(trampoline + sizeof(angle_bytes), reinterpret_cast<uintptr_t>(trampoline) + sizeof(angle_bytes),
                input_angle + sizeof(angle_bytes), 0xe9);
    DWORD old;
    if (!VirtualProtect(trampoline, 64, PAGE_EXECUTE_READ, &old)) return 0;
    FlushInstructionCache(GetCurrentProcess(), trampoline, 64);
    original_angle = reinterpret_cast<AngleFn>(trampoline);
    for (const auto& hook : presentation_hooks) {
        if (!patch_branch(hook.site, hook.replacement, 0xe8)) {
            log("Unable to enable the HUD and room renderer hooks. Close the game before retrying.");
            return 0;
        }
    }
    if (!patch_branch(input_angle, reinterpret_cast<uintptr_t>(PlayerAngle), 0xe9)
        || !patch_branch(0x492832, reinterpret_cast<uintptr_t>(UpdateBlur), 0xe8)
        || !patch_branch(0x4928d2, reinterpret_cast<uintptr_t>(UpdateShadows), 0xe8)
        || !patch_branch(0x4928d7, reinterpret_cast<uintptr_t>(UpdateCharacters), 0xe8)
        || !patch_branch(0x43590a, reinterpret_cast<uintptr_t>(KeepPlayerOnScreen), 0xe8)
        || !patch_branch(0x4b6f1a, reinterpret_cast<uintptr_t>(KeepPlayerOnScreen), 0xe8)
        || !patch_branch(0x49257d, reinterpret_cast<uintptr_t>(UpdateObjectDistances), 0xe8)
        || !patch_branch(automatic_dropout, reinterpret_cast<uintptr_t>(AutomaticDropoutEntry), 0xe9)
        || !patch_branch(player_trigger_node, reinterpret_cast<uintptr_t>(EitherLocalPlayerInLevelNode), 0xe9)
        || !patch_branch(player_trigger_area, reinterpret_cast<uintptr_t>(EitherLocalPlayerInTriggerArea), 0xe9)
        || !patch_branch(pickup_camera_load, reinterpret_cast<uintptr_t>(PickupCameraEntry), 0xe9)
        || !patch_branch(force_target_clip, reinterpret_cast<uintptr_t>(ForceTargetClip), 0xe8)
        || !patch_branch(sustain_force_clip, reinterpret_cast<uintptr_t>(SustainForceEntry), 0xe9)
        || !patch_branch(0x41ecc2, reinterpret_cast<uintptr_t>(DrawForceGlow), 0xe8)
        || !patch_branch(0x41ed49, reinterpret_cast<uintptr_t>(DrawForceGlow), 0xe8)
        || !patch_branch(0x41f5ed, reinterpret_cast<uintptr_t>(DrawForceGlow), 0xe8)
        || !patch_branch(0x4a4ebf, reinterpret_cast<uintptr_t>(OrientGlow), 0xe8)
        || !patch_branch(0x4a4f95, reinterpret_cast<uintptr_t>(OrientGlow), 0xe8)
        || !patch_branch(offscreen_bolt, reinterpret_cast<uintptr_t>(OffscreenBoltEntry), 0xe9)
        || !patch_branch(0x492882, reinterpret_cast<uintptr_t>(DrawPartsForViews), 0xe8)
        || !patch_branch(0x64f751, reinterpret_cast<uintptr_t>(CullPartFromView), 0xe8)
        || !patch_branch(scene_loop, reinterpret_cast<uintptr_t>(SceneEntry), 0xe8)
        || !patch_branch(0x60b513, reinterpret_cast<uintptr_t>(AfterScenes), 0xe8)
        || !patch_branch(keyboard_cooperative_call, reinterpret_cast<uintptr_t>(ConfigureGameKeyboard), 0xe8)
        || !patch_branch(0x60b56e, reinterpret_cast<uintptr_t>(EndFrameInput), 0xe8)
        || !patch_branch(0x60b57a, reinterpret_cast<uintptr_t>(PresentOrCapture), 0xe8)
        || !patch_branch(render_call, reinterpret_cast<uintptr_t>(RenderEntry), 0xe8)) {
        log("Unable to enable all hooks. Close the game before retrying.");
        return 0;
    }
    log("Camera and world renderer hooks active. Waiting for two local players.");
    log(enable_display_menu() ? "Display Mode enabled in Video Settings."
                              : "Display Mode menu hooks did not match this game.");
    return 1;
}
