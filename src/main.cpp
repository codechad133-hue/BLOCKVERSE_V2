// BLOCKVERSE 2.0 - voxel sandbox (raylib + ENet)

// ENet pulls in <winsock2.h>/<windows.h> on Windows, whose GDI/USER APIs
// (Rectangle, CloseWindow, ShowCursor, DrawText...) clash with raylib, and whose
// min/max macros clash with std::min/std::max. Strip them and include ENet FIRST.
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define NOGDI
#define NOUSER
#endif
#include <enet/enet.h>
#include "raylib.h"
#include "raymath.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace bv {
constexpr int WX = 128, WZ = 128, WY = 48, CS = 16, PORT = 27125, MAX_PLAYERS = 16;
constexpr float EYE = 1.7f, RADIUS = 0.3f, BODY_H = 1.8f;
enum Block : uint8_t { Air, Grass, Dirt, Stone, Sand, Wood, Leaves, Coal, Iron, Crystal, Water, Bedrock };
enum Mode : uint8_t { Survival, Creative, Hardcore };
enum Packet : uint8_t { Hello = 1, PlayerState, BlockEdit, Chat, WorldSeed, Leave };

// Wire formats (explicitly packed so both ends agree regardless of compiler padding).
#pragma pack(push, 1)
struct HelloMsg { uint8_t type; uint32_t id; };
struct StateMsg { uint8_t type; uint32_t id; float x, y, z, yaw; uint8_t health; };
struct EditMsg  { uint8_t type; int16_t x, y, z; uint8_t block; };
struct LeaveMsg { uint8_t type; uint32_t id; };
#pragma pack(pop)

struct PlayerNet { uint32_t id; Vector3 pos; float yaw; uint8_t health; };
struct Mob { Vector3 p; float hp = 30, attack = 0; bool boss = false, alive = true; };
struct Chunk { Model model{}; bool ready = false; };

static uint8_t world[WX][WY][WZ];
static Chunk chunks[WX / CS][WZ / CS];
static bool dirty[WX / CS][WZ / CS];
static int seed = 20261122;

static int terrain(int x, int z) {
  float a = 7 + 3.5f * sinf((x + seed % 31) * .055f) + 2.5f * cosf((z + seed % 17) * .061f) + 1.6f * sinf((x + z) * .12f);
  return std::clamp((int)a, 3, WY - 10);
}
static Color color(Block b) {
  switch (b) {
    case Grass:   return {92, 174, 76, 255};
    case Dirt:    return {126, 86, 55, 255};
    case Stone:   return {122, 130, 140, 255};
    case Sand:    return {218, 198, 132, 255};
    case Wood:    return {126, 83, 48, 255};
    case Leaves:  return {63, 137, 66, 255};
    case Coal:    return {54, 58, 65, 255};
    case Iron:    return {190, 150, 112, 255};
    case Crystal: return {60, 210, 220, 255};
    case Water:   return {45, 115, 205, 180};
    case Bedrock: return {38, 40, 46, 255};
    default:      return BLANK;
  }
}
static bool inRange(int x, int y, int z) { return x >= 0 && x < WX && y >= 0 && y < WY && z >= 0 && z < WZ; }
static bool solid(int x, int y, int z) { return inRange(x, y, z) && world[x][y][z] != Air && world[x][y][z] != Water; }

// Top surface height (y of the empty cell above the highest solid block at or below y).
static int groundBelow(int x, int y, int z) {
  x = std::clamp(x, 0, WX - 1); z = std::clamp(z, 0, WZ - 1);
  for (int yy = std::min(y, WY - 1); yy >= 0; yy--) if (solid(x, yy, z)) return yy + 1;
  return 0;
}

static void freeChunks() {
  for (auto& a : chunks) for (auto& c : a) if (c.ready) { UnloadModel(c.model); c.ready = false; }
}
static void buildChunk(int cx, int cz) {
  auto& c = chunks[cx][cz];
  if (c.ready) { UnloadModel(c.model); c.ready = false; }
  std::vector<float> v;
  std::vector<unsigned char> col;
  std::vector<unsigned short> ix;
  // Each quad is wound counter-clockwise when seen from outside (raylib culls back faces).
  const Vector3 f[6][4] = {
    {{0, 1, 1}, {1, 1, 1}, {1, 1, 0}, {0, 1, 0}},  // +y
    {{0, 0, 0}, {1, 0, 0}, {1, 0, 1}, {0, 0, 1}},  // -y
    {{1, 0, 1}, {1, 0, 0}, {1, 1, 0}, {1, 1, 1}},  // +x
    {{0, 0, 0}, {0, 0, 1}, {0, 1, 1}, {0, 1, 0}},  // -x
    {{0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}},  // +z
    {{1, 0, 0}, {0, 0, 0}, {0, 1, 0}, {1, 1, 0}}}; // -z
  const int dx[] = {0, 0, 1, -1, 0, 0}, dy[] = {1, -1, 0, 0, 0, 0}, dz[] = {0, 0, 0, 0, 1, -1};
  for (int x = cx * CS; x < (cx + 1) * CS; x++)
    for (int z = cz * CS; z < (cz + 1) * CS; z++)
      for (int y = 0; y < WY; y++) {
        Block b = (Block)world[x][y][z];
        if (b == Air || b == Water) continue;
        for (int k = 0; k < 6; k++) {
          if (v.size() / 3 > 65520) break;  // 16-bit index limit; never hit by normal terrain
          if (solid(x + dx[k], y + dy[k], z + dz[k])) continue;
          Color cc = color(b);
          float sh = (k == 1) ? .72f : (k >= 2 ? .86f : 1.f);
          cc.r = (unsigned char)(cc.r * sh); cc.g = (unsigned char)(cc.g * sh); cc.b = (unsigned char)(cc.b * sh);
          unsigned short s = (unsigned short)(v.size() / 3);
          for (int q = 0; q < 4; q++) {
            v.insert(v.end(), {x + f[k][q].x, y + f[k][q].y, z + f[k][q].z});
            col.insert(col.end(), {cc.r, cc.g, cc.b, cc.a});
          }
          ix.insert(ix.end(), {s, (unsigned short)(s + 1), (unsigned short)(s + 2), s, (unsigned short)(s + 2), (unsigned short)(s + 3)});
        }
      }
  if (ix.empty()) return;
  Mesh m{};
  m.vertexCount = (int)v.size() / 3;
  m.triangleCount = (int)ix.size() / 3;
  m.vertices = (float*)MemAlloc((unsigned)(v.size() * sizeof(float)));
  memcpy(m.vertices, v.data(), v.size() * sizeof(float));
  m.colors = (unsigned char*)MemAlloc((unsigned)col.size());
  memcpy(m.colors, col.data(), col.size());
  m.indices = (unsigned short*)MemAlloc((unsigned)(ix.size() * sizeof(unsigned short)));
  memcpy(m.indices, ix.data(), ix.size() * sizeof(unsigned short));
  UploadMesh(&m, false);
  c.model = LoadModelFromMesh(m);
  c.ready = true;
}
// Edits only flag chunks; they are rebuilt once per frame (and only the chunks that changed).
static void markDirty(int x, int z) {
  int cx = x / CS, cz = z / CS;
  dirty[cx][cz] = true;
  if (x % CS == 0 && cx > 0) dirty[cx - 1][cz] = true;
  if (x % CS == CS - 1 && cx < WX / CS - 1) dirty[cx + 1][cz] = true;
  if (z % CS == 0 && cz > 0) dirty[cx][cz - 1] = true;
  if (z % CS == CS - 1 && cz < WZ / CS - 1) dirty[cx][cz + 1] = true;
}
static void setBlock(int x, int y, int z, uint8_t b) {
  if (!inRange(x, y, z)) return;
  world[x][y][z] = b;
  markDirty(x, z);
}
static void rebuildDirty() {
  for (int x = 0; x < WX / CS; x++) for (int z = 0; z < WZ / CS; z++) if (dirty[x][z]) { dirty[x][z] = false; buildChunk(x, z); }
}
static void generate() {
  memset(world, 0, sizeof(world));  // a previous world's edits must not leak into a new one
  memset(dirty, 0, sizeof(dirty));
  for (int x = 0; x < WX; x++) for (int z = 0; z < WZ; z++) {
    int h = terrain(x, z);
    for (int y = 0; y <= h; y++) {
      Block b = y == 0 ? Bedrock : (y == h ? (h < 7 ? Sand : Grass) : (y > h - 4 ? Dirt : Stone));
      if (y < h - 5 && (x * 17 + z * 31 + y * 13) % 131 == 0) b = Coal;
      if (y < h - 7 && (x * 11 + z * 19 + y * 23) % 223 == 0) b = Iron;
      if (y < h - 9 && (x * 7 + z * 29 + y * 17) % 997 == 0) b = Crystal;
      world[x][y][z] = b;
    }
    if (h > 9 && (x * 17 + z * 31) % 79 == 0) {
      for (int y = h + 1; y < h + 5; y++) world[x][y][z] = Wood;
      for (int xx = -2; xx <= 2; xx++) for (int zz = -2; zz <= 2; zz++) for (int yy = h + 3; yy <= h + 6; yy++) {
        int ax = x + xx, az = z + zz;
        if (ax >= 0 && ax < WX && az >= 0 && az < WZ && yy < WY && std::abs(xx) + std::abs(zz) + (yy == h + 6 ? 1 : 0) < 5 && world[ax][yy][az] == Air)
          world[ax][yy][az] = Leaves;
      }
    }
  }
  for (int x = 0; x < WX / CS; x++) for (int z = 0; z < WZ / CS; z++) buildChunk(x, z);
}

// Save format: "BVW1", seed, world blocks, then (optional trailer) game mode byte.
static void saveWorld(const std::string& path, Mode mode) {
  std::ofstream f(path, std::ios::binary);
  if (!f) return;
  f.write("BVW1", 4);
  f.write((const char*)&seed, sizeof(seed));
  f.write((const char*)world, sizeof(world));
  uint8_t m = mode;
  f.write((const char*)&m, 1);
}
static bool loadWorld(const std::string& path, Mode& mode) {
  std::ifstream f(path, std::ios::binary);
  char magic[4];
  if (!f.read(magic, 4) || memcmp(magic, "BVW1", 4)) return false;
  int s;
  if (!f.read((char*)&s, sizeof(s))) return false;
  std::vector<uint8_t> tmp(sizeof(world));  // read fully first so a truncated file can't corrupt the live world
  if (!f.read((char*)tmp.data(), (std::streamsize)tmp.size())) return false;
  uint8_t m;
  if (f.read((char*)&m, 1) && m <= Hardcore) mode = (Mode)m;
  seed = s;
  memcpy(world, tmp.data(), sizeof(world));
  memset(dirty, 0, sizeof(dirty));
  for (int x = 0; x < WX / CS; x++) for (int z = 0; z < WZ / CS; z++) buildChunk(x, z);
  return true;
}

// ---------------------------------------------------------------- networking
static bool enetUp = false;
static bool enetStart() {
  if (!enetUp && enet_initialize() == 0) enetUp = true;
  return enetUp;
}
struct NetClient {
  ENetHost* host = nullptr;
  ENetPeer* peer = nullptr;
  bool connected = false;
  uint32_t id = 0;
  std::array<PlayerNet, MAX_PLAYERS> others{};
  int count = 0;
};
static void sendPacket(NetClient& n, const void* data, size_t size, bool reliable) {
  if (!n.peer || !n.connected) return;
  ENetPacket* p = enet_packet_create(data, size, reliable ? ENET_PACKET_FLAG_RELIABLE : 0);
  enet_peer_send(n.peer, 0, p);
}
static void disconnectNet(NetClient& n) {
  if (n.peer) { if (n.connected) enet_peer_disconnect_now(n.peer, 0); else enet_peer_reset(n.peer); }
  if (n.host) enet_host_destroy(n.host);
  n.host = nullptr; n.peer = nullptr; n.connected = false; n.id = 0; n.count = 0;
}
static bool connectNet(NetClient& n, const char* ip) {
  disconnectNet(n);  // no leaked hosts on repeated attempts
  if (!enetStart()) return false;
  n.host = enet_host_create(nullptr, 1, 2, 0, 0);
  if (!n.host) return false;
  ENetAddress a{};
  if (enet_address_set_host(&a, ip) != 0) { disconnectNet(n); return false; }
  a.port = PORT;
  n.peer = enet_host_connect(n.host, &a, 2, 0);
  if (!n.peer) { disconnectNet(n); return false; }
  ENetEvent e;
  if (enet_host_service(n.host, &e, 3000) > 0 && e.type == ENET_EVENT_TYPE_CONNECT) { n.connected = true; return true; }
  disconnectNet(n);
  return false;
}
static void removeOther(NetClient& n, uint32_t id) {
  for (int i = 0; i < n.count; i++) if (n.others[i].id == id) { n.others[i] = n.others[n.count - 1]; n.count--; return; }
}
static void handlePacket(NetClient& n, const ENetPacket* pk) {
  const uint8_t* d = pk->data;
  size_t len = pk->dataLength;
  if (len == 0) return;
  if (d[0] == Hello && len == sizeof(HelloMsg)) {
    HelloMsg h; memcpy(&h, d, sizeof(h)); n.id = h.id;
  } else if (d[0] == PlayerState && len == sizeof(StateMsg)) {
    StateMsg s; memcpy(&s, d, sizeof(s));
    if (s.id == n.id) return;
    PlayerNet p{s.id, {s.x, s.y, s.z}, s.yaw, s.health};
    for (int i = 0; i < n.count; i++) if (n.others[i].id == p.id) { n.others[i] = p; return; }
    if (n.count < MAX_PLAYERS) n.others[n.count++] = p;
  } else if (d[0] == BlockEdit && len == sizeof(EditMsg)) {
    EditMsg e; memcpy(&e, d, sizeof(e));
    if (e.block <= Bedrock) setBlock(e.x, e.y, e.z, e.block);
  } else if (d[0] == Leave && len == sizeof(LeaveMsg)) {
    LeaveMsg l; memcpy(&l, d, sizeof(l)); removeOther(n, l.id);
  }
}
static void pollNet(NetClient& n) {
  if (!n.host) return;
  ENetEvent e;
  while (enet_host_service(n.host, &e, 0) > 0) {
    if (e.type == ENET_EVENT_TYPE_RECEIVE) { handlePacket(n, e.packet); enet_packet_destroy(e.packet); }
    else if (e.type == ENET_EVENT_TYPE_DISCONNECT) { n.connected = false; n.count = 0; }
  }
}

static volatile std::sig_atomic_t serverRunning = 1;
static void onSignal(int) { serverRunning = 0; }
static void relay(ENetHost* h, ENetPeer* from, ENetPacket* pk) {
  if (pk->dataLength < 1) return;
  const uint8_t t = pk->data[0];
  std::vector<uint8_t> out(pk->data, pk->data + pk->dataLength);
  bool reliable = true;
  if (t == PlayerState && pk->dataLength == sizeof(StateMsg)) {
    StateMsg s; memcpy(&s, out.data(), sizeof(s));
    s.id = (uint32_t)(uintptr_t)from->data;  // server decides identity; clients can't spoof it
    memcpy(out.data(), &s, sizeof(s));
    reliable = false;
  } else if (t == BlockEdit && pk->dataLength == sizeof(EditMsg)) {
    EditMsg e; memcpy(&e, out.data(), sizeof(e));
    if (!inRange(e.x, e.y, e.z) || e.block > Bedrock) return;
  } else return;
  for (size_t i = 0; i < h->peerCount; i++) {
    ENetPeer* p = &h->peers[i];
    if (p == from || p->state != ENET_PEER_STATE_CONNECTED) continue;  // no echo back to the sender
    enet_peer_send(p, 0, enet_packet_create(out.data(), out.size(), reliable ? ENET_PACKET_FLAG_RELIABLE : 0));
  }
}
static void runServer() {
  if (!enetStart()) { std::cerr << "ENet init failed\n"; return; }
  ENetAddress addr{};
  addr.host = ENET_HOST_ANY;
  addr.port = PORT;
  ENetHost* server = enet_host_create(&addr, MAX_PLAYERS, 2, 0, 0);
  if (!server) { std::cerr << "Could not start server\n"; enet_deinitialize(); return; }
  std::signal(SIGINT, onSignal);
  std::signal(SIGTERM, onSignal);
  std::cout << "BLOCKVERSE dedicated server listening on UDP " << PORT << " (Ctrl+C to stop)\n";
  uint32_t nextId = 1;
  while (serverRunning) {
    ENetEvent ev;
    while (enet_host_service(server, &ev, 10) > 0) {
      if (ev.type == ENET_EVENT_TYPE_CONNECT) {
        uint32_t id = nextId++;
        ev.peer->data = (void*)(uintptr_t)id;
        HelloMsg h{(uint8_t)Hello, id};
        enet_peer_send(ev.peer, 0, enet_packet_create(&h, sizeof(h), ENET_PACKET_FLAG_RELIABLE));
        std::cout << "Player " << id << " connected\n";
      } else if (ev.type == ENET_EVENT_TYPE_RECEIVE) {
        relay(server, ev.peer, ev.packet);
        enet_packet_destroy(ev.packet);
      } else if (ev.type == ENET_EVENT_TYPE_DISCONNECT) {
        uint32_t id = (uint32_t)(uintptr_t)ev.peer->data;
        ev.peer->data = nullptr;
        LeaveMsg l{(uint8_t)Leave, id};
        for (size_t i = 0; i < server->peerCount; i++)
          if (&server->peers[i] != ev.peer && server->peers[i].state == ENET_PEER_STATE_CONNECTED)
            enet_peer_send(&server->peers[i], 0, enet_packet_create(&l, sizeof(l), ENET_PACKET_FLAG_RELIABLE));
        std::cout << "Player " << id << " disconnected\n";
      }
    }
  }
  enet_host_destroy(server);
  enet_deinitialize();
  enetUp = false;
}

static Vector3 rayDir(float yaw, float pitch) { return {sinf(yaw) * cosf(pitch), sinf(pitch), -cosf(yaw) * cosf(pitch)}; }

// Player collision: `f` is the feet position; the body is a RADIUS-wide, BODY_H-tall box.
static bool hits(Vector3 f) {
  const float hs[3] = {0.f, 1.0f, 1.75f};
  for (int i = 0; i < 4; i++) {
    float ox = (i & 1) ? RADIUS : -RADIUS, oz = (i & 2) ? RADIUS : -RADIUS;
    for (float h : hs) if (solid((int)floorf(f.x + ox), (int)floorf(f.y + h), (int)floorf(f.z + oz))) return true;
  }
  return false;
}
}  // namespace bv


// Per-user data folder (%APPDATA%\\BLOCKVERSE) so saves/settings work when installed under Program Files.
static std::string dataDir() {
  const char* base = std::getenv("APPDATA");
  if (!base) base = std::getenv("HOME");
  std::string d = base ? std::string(base) + "/BLOCKVERSE" : ".";
  std::error_code ec; std::filesystem::create_directories(d, ec);
  return ec ? "." : d;
}
struct Settings { bool fullscreen = true, vsync = true, showFps = false; int renderDistance = 4; float sens = .0025f, fov = 75.f; };
static void saveSettings(const std::string& path, const Settings& c) {
  std::ofstream f(path);
  f << c.fullscreen << ' ' << c.vsync << ' ' << c.showFps << ' ' << c.renderDistance << ' ' << c.sens << ' ' << c.fov << '\n';
}
static void loadSettings(const std::string& path, Settings& c) {
  std::ifstream f(path); Settings t;
  if (f >> t.fullscreen >> t.vsync >> t.showFps >> t.renderDistance >> t.sens >> t.fov) {
    t.renderDistance = std::clamp(t.renderDistance, 2, 8);
    t.sens = std::clamp(t.sens, .0008f, .008f);
    t.fov = std::clamp(t.fov, 60.f, 110.f);
    c = t;
  }
}

using namespace bv;

int main(int argc, char** argv) {
  std::string serverIp = "127.0.0.1", status = "Offline";
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "--server") { runServer(); return 0; }
    if (a == "--connect" && i + 1 < argc) serverIp = argv[++i];
  }
  const std::string worldPath = dataDir() + "/blockverse_world.bvw", cfgPath = dataDir() + "/settings.cfg";
  Settings cfg; loadSettings(cfgPath, cfg);
  SetConfigFlags(FLAG_WINDOW_RESIZABLE | (cfg.vsync ? FLAG_VSYNC_HINT : 0) | FLAG_MSAA_4X_HINT);
  InitWindow(1440, 900, "BLOCKVERSE | Beyond the Blocks");
  SetExitKey(KEY_NULL);  // raylib's default makes ESC close the window; ESC is "save and go to title" here
  SetTargetFPS(cfg.vsync ? 144 : 240);
  if (cfg.fullscreen) ToggleBorderlessWindowed();

  int screen = 0, settingsFrom = 0;  // 0 title, 1 world, 2 settings, 3 crafting
  bool quit = false, worldLoaded = false;
  Mode mode = Survival;
  Camera3D cam{{64, 22, 64}, {64, 22, 63}, {0, 1, 0}, 75, CAMERA_PERSPECTIVE};
  Vector3 feet = {64.5f, 22, 64.5f};
  int selected = 0, health = 100, hunger = 100, xp = 0, level = 1, skipMouse = 0;
  float yaw = 0, pitch = 0, vy = 0, stamina = 100, day = 0, spawnTimer = 0, regen = 0, netTimer = 0;
  bool grounded = false, vsync = cfg.vsync, showFps = cfg.showFps, bossSpawned = false;
  int renderDistance = cfg.renderDistance;
  float mouseSens = cfg.sens, fov = cfg.fov;
  int inv[12];  // 0-8 hotbar blocks, 10 = stone pickaxes, 11 = crystal blades
  const Block bar[12] = {Grass, Dirt, Stone, Sand, Wood, Leaves, Coal, Iron, Crystal, Water, Air, Bedrock};
  std::vector<Mob> mobs;
  NetClient net;

  auto capture = [&](bool on) { if (on) DisableCursor(); else EnableCursor(); skipMouse = 2; };
  auto gainXp = [&](int a) { xp += a; while (xp >= level * 100) { xp -= level * 100; level++; } };
  auto placePlayer = [&]() {
    feet = {64.5f, (float)groundBelow(64, WY - 1, 64), 64.5f};
    vy = 0; grounded = false;
  };
  auto resetPlayer = [&]() {
    const int start[12] = {24, 32, 20, 0, 12, 8, 0, 0, 0, 0, 0, 0};
    memcpy(inv, start, sizeof(inv));
    health = 100; hunger = 100; xp = 0; level = 1; stamina = 100; selected = 0;
    mobs.clear(); bossSpawned = false; spawnTimer = 0;
    placePlayer();
  };
  auto enterWorld = [&]() { worldLoaded = true; screen = 1; capture(true); };

  auto persist = [&]() {
    cfg.fullscreen = IsWindowState(FLAG_BORDERLESS_WINDOWED_MODE); cfg.vsync = vsync; cfg.showFps = showFps;
    cfg.renderDistance = renderDistance; cfg.sens = mouseSens; cfg.fov = fov; saveSettings(cfgPath, cfg);
  };
  auto toggleFullscreen = [&]() { ToggleBorderlessWindowed(); skipMouse = 2; };

  while (!quit && !WindowShouldClose()) {
    if (IsKeyPressed(KEY_F11) || ((IsKeyDown(KEY_LEFT_ALT) || IsKeyDown(KEY_RIGHT_ALT)) && IsKeyPressed(KEY_ENTER))) toggleFullscreen();
    cam.fovy = fov;
    float dt = std::min(GetFrameTime(), .04f);

    // ------------------------------------------------ title / settings input
    if (screen == 0) {
      if (IsKeyPressed(KEY_ONE) || IsKeyPressed(KEY_TWO) || IsKeyPressed(KEY_THREE)) {
        mode = IsKeyPressed(KEY_ONE) ? Survival : IsKeyPressed(KEY_TWO) ? Creative : Hardcore;
        generate(); resetPlayer(); enterWorld();
      } else if (IsKeyPressed(KEY_FOUR)) { settingsFrom = 0; screen = 2; }
      else if (IsKeyPressed(KEY_FIVE)) {
        if (loadWorld(worldPath, mode)) { resetPlayer(); enterWorld(); }
        else status = "No save found";
      } else if (IsKeyPressed(KEY_SIX)) {
        status = connectNet(net, serverIp.c_str()) ? "Connected to " + serverIp + " - now start or load a world"
                                                   : "Connection failed: check server/IP/UDP " + std::to_string(PORT);
      } else if (IsKeyPressed(KEY_ESCAPE)) quit = true;
    } else if (screen == 2) {
      if (IsKeyPressed(KEY_V)) {
        vsync = !vsync;
        if (vsync) SetWindowState(FLAG_VSYNC_HINT); else ClearWindowState(FLAG_VSYNC_HINT);
        SetTargetFPS(vsync ? 144 : 240);
      }
      if (IsKeyPressed(KEY_F)) showFps = !showFps;
      if (IsKeyPressed(KEY_W)) toggleFullscreen();
      if (IsKeyPressed(KEY_COMMA)) fov = std::max(60.f, fov - 5.f);
      if (IsKeyPressed(KEY_PERIOD)) fov = std::min(110.f, fov + 5.f);
      if (IsKeyPressed(KEY_LEFT_BRACKET)) renderDistance = std::max(2, renderDistance - 1);
      if (IsKeyPressed(KEY_RIGHT_BRACKET)) renderDistance = std::min(8, renderDistance + 1);
      if (IsKeyPressed(KEY_MINUS)) mouseSens = std::max(.0008f, mouseSens - .0003f);
      if (IsKeyPressed(KEY_EQUAL)) mouseSens = std::min(.008f, mouseSens + .0003f);
      if (IsKeyPressed(KEY_ESCAPE)) { persist(); screen = settingsFrom; if (screen == 1) capture(true); }  // back to where we came from
    }
    if (screen == 0 || screen == 2) {
      BeginDrawing();
      ClearBackground({10, 16, 28, 255});
      DrawText("BLOCKVERSE", 65, 65, 58, RAYWHITE);
      DrawText("BEYOND THE BLOCKS", 70, 130, 20, {110, 200, 255, 255});
      if (screen == 0) {
        DrawText("A procedural voxel survival universe", 70, 195, 20, LIGHTGRAY);
        DrawRectangleRounded({70, 280, 310, 60}, .18f, 8, {40, 110, 180, 255}); DrawText("1  SURVIVAL", 95, 298, 24, RAYWHITE);
        DrawRectangleRounded({70, 355, 310, 60}, .18f, 8, {35, 120, 90, 255});  DrawText("2  CREATIVE", 95, 373, 24, RAYWHITE);
        DrawRectangleRounded({70, 430, 310, 60}, .18f, 8, {150, 65, 65, 255});  DrawText("3  HARDCORE", 95, 448, 24, RAYWHITE);
        DrawText("4  SETTINGS", 95, 540, 21, RAYWHITE);
        DrawText("5  LOAD WORLD", 95, 580, 21, RAYWHITE);
        DrawText(TextFormat("6  CONNECT ONLINE (%s)", serverIp.c_str()), 95, 620, 21, RAYWHITE);
        DrawText("ESC  QUIT", 95, 660, 21, RAYWHITE);
        DrawText("WASD | Mouse | Space jump | LMB mine | RMB build | F11 fullscreen", 70, 760, 17, LIGHTGRAY);
      } else {
        DrawText("PC SETTINGS", 70, 210, 32, RAYWHITE);
        DrawText(TextFormat("[V] VSync: %s", vsync ? "ON" : "OFF"), 70, 290, 22, RAYWHITE);
        DrawText(TextFormat("[F] FPS counter: %s", showFps ? "ON" : "OFF"), 70, 335, 22, RAYWHITE);
        DrawText(TextFormat("[ / ] Render distance: %d chunks", renderDistance), 70, 380, 22, RAYWHITE);
        DrawText(TextFormat("[- / +] Mouse sensitivity: %.4f", mouseSens), 70, 425, 22, RAYWHITE);
        DrawText(TextFormat("[W] Fullscreen (F11): %s", IsWindowState(FLAG_BORDERLESS_WINDOWED_MODE) ? "ON" : "OFF"), 70, 470, 22, RAYWHITE);
        DrawText(TextFormat("[, / .] Field of view: %d", (int)fov), 70, 515, 22, RAYWHITE);
        DrawText("ESC to go back", 70, 580, 20, YELLOW);
      }
      DrawText(status.c_str(), 70, 700, 17, GOLD);
      EndDrawing();
      continue;
    }

    // ------------------------------------------------ in-world (1) / crafting (3)
    if (IsKeyPressed(KEY_ESCAPE)) {
      if (screen == 3) screen = 1;
      else { saveWorld(worldPath, mode); screen = 0; capture(false); continue; }
    }
    if (IsKeyPressed(KEY_TAB)) { settingsFrom = 1; screen = 2; capture(false); continue; }
    if (IsKeyPressed(KEY_C)) screen = (screen == 3) ? 1 : 3;
    if (IsKeyPressed(KEY_F5) && screen == 1) { saveWorld(worldPath, mode); status = "World saved"; }

    int hx = -1, hy = -1, hz = -1;
    if (screen == 1) {
      for (int i = 0; i < 9; i++) if (IsKeyPressed(KEY_ONE + i)) selected = i;
      if (skipMouse > 0) skipMouse--;  // ignore the cursor-recentering jump after (re)capturing the mouse
      else { Vector2 md = GetMouseDelta(); yaw += md.x * mouseSens; pitch = std::clamp(pitch - md.y * mouseSens, -1.48f, 1.48f); }

      // movement with real collision
      Vector3 fwd = rayDir(yaw, 0), right = {cosf(yaw), 0, sinf(yaw)}, move = {0, 0, 0};
      if (IsKeyDown(KEY_W)) move = Vector3Add(move, fwd);
      if (IsKeyDown(KEY_S)) move = Vector3Subtract(move, fwd);
      if (IsKeyDown(KEY_D)) move = Vector3Add(move, right);
      if (IsKeyDown(KEY_A)) move = Vector3Subtract(move, right);
      bool moving = Vector3Length(move) > 0;
      if (moving) move = Vector3Normalize(move);
      float speed = 5;
      if (moving && IsKeyDown(KEY_LEFT_SHIFT) && stamina > 0) { speed = 8; stamina = std::max(0.0f, stamina - 26 * dt); }
      else stamina = std::min(100.0f, stamina + 18 * dt);
      Vector3 t = feet; t.x += move.x * speed * dt; if (!hits(t)) feet.x = t.x;
      t = feet;         t.z += move.z * speed * dt; if (!hits(t)) feet.z = t.z;
      feet.x = std::clamp(feet.x, RADIUS + .01f, (float)WX - RADIUS - .01f);
      feet.z = std::clamp(feet.z, RADIUS + .01f, (float)WZ - RADIUS - .01f);
      if (IsKeyPressed(KEY_SPACE) && grounded) vy = 7;
      vy = std::max(vy - 20 * dt, -20.0f);
      grounded = false;
      float oy = feet.y;
      feet.y += vy * dt;
      if (hits(feet)) {
        if (vy < 0) { feet.y = floorf(feet.y) + 1; if (hits(feet)) feet.y = oy; else grounded = true; }
        else feet.y = oy;  // bumped head
        vy = 0;
      }
      cam.position = {feet.x, feet.y + EYE, feet.z};
      Vector3 dir = rayDir(yaw, pitch);
      cam.target = Vector3Add(cam.position, dir);

      // block targeting
      int nx = -1, ny = -1, nz = -1;
      float hitT = 1e9f;
      Vector3 prev = cam.position;
      for (float tt = .15f; tt < 7; tt += .08f) {
        Vector3 p = Vector3Add(cam.position, Vector3Scale(dir, tt));
        int x = (int)floorf(p.x), y = (int)floorf(p.y), z = (int)floorf(p.z);
        if (solid(x, y, z)) { hx = x; hy = y; hz = z; nx = (int)floorf(prev.x); ny = (int)floorf(prev.y); nz = (int)floorf(prev.z); hitT = tt; break; }
        prev = p;
      }

      // left click: hit the mob you are aiming at, otherwise mine the targeted block
      Mob* target = nullptr;
      float best = 4.5f;
      for (auto& m : mobs) {
        if (!m.alive) continue;
        Vector3 d = Vector3Subtract(m.p, cam.position);
        float dist = Vector3Length(d);
        if (dist > .01f && dist < best && dist < hitT + .5f && Vector3DotProduct(Vector3Scale(d, 1 / dist), dir) > .93f) { best = dist; target = &m; }
      }
      if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        if (target) {
          target->hp -= (target->boss ? 12.f : 25.f) * (inv[11] > 0 ? 2.f : 1.f);  // crystal blade doubles damage
          if (target->hp <= 0) { target->alive = false; gainXp(target->boss ? 500 : 20); }
        } else if (hx >= 0 && world[hx][hy][hz] != Bedrock) {
          Block b = (Block)world[hx][hy][hz];
          setBlock(hx, hy, hz, Air);
          if (mode != Creative) {
            int i = (int)b - 1;
            if (i >= 0 && i <= 8) inv[i] += inv[10] > 0 ? 2 : 1;  // stone pickaxe doubles drops
            gainXp(10);
          }
          EditMsg e{(uint8_t)BlockEdit, (int16_t)hx, (int16_t)hy, (int16_t)hz, (uint8_t)Air};
          sendPacket(net, &e, sizeof(e), true);
        }
      }
      // right click: place (never inside the player)
      if (hx >= 0 && IsMouseButtonPressed(MOUSE_BUTTON_RIGHT) && bar[selected] != Air && inRange(nx, ny, nz) && world[nx][ny][nz] == Air && ny > 0 &&
          (mode == Creative || inv[selected] > 0)) {
        bool overlap = nx + 1 > feet.x - RADIUS && nx < feet.x + RADIUS && nz + 1 > feet.z - RADIUS && nz < feet.z + RADIUS &&
                       ny + 1 > feet.y && ny < feet.y + BODY_H;
        if (!overlap) {
          setBlock(nx, ny, nz, bar[selected]);
          if (mode != Creative) inv[selected]--;
          EditMsg e{(uint8_t)BlockEdit, (int16_t)nx, (int16_t)ny, (int16_t)nz, (uint8_t)bar[selected]};
          sendPacket(net, &e, sizeof(e), true);
        }
      }

      // creatures: time-based spawning, ground-following pursuit, boss on B after level 5
      int aliveCount = 0;
      for (auto& m : mobs) if (m.alive) aliveCount++;
      spawnTimer += dt;
      if (spawnTimer > 6 && aliveCount < 14) {
        spawnTimer = 0;
        float a = (float)GetRandomValue(0, 628) / 100.0f;
        float sx = std::clamp(feet.x + cosf(a) * 18, 2.0f, 125.0f), sz = std::clamp(feet.z + sinf(a) * 18, 2.0f, 125.0f);
        mobs.push_back({{sx, groundBelow((int)sx, (int)feet.y + 3, (int)sz) + .6f, sz}, 30, 0, false, true});  // scan from near player height: never spawn on a tree canopy
      }
      if (IsKeyPressed(KEY_B) && level >= 5 && !bossSpawned) {
        float bx = std::clamp(feet.x + 8, 2.0f, 125.0f), bz = std::clamp(feet.z + 8, 2.0f, 125.0f);
        mobs.push_back({{bx, groundBelow((int)bx, (int)feet.y + 3, (int)bz) + 1.5f, bz}, 500, 0, true, true});
        bossSpawned = true;
      }
      Vector3 body = {feet.x, feet.y + .9f, feet.z};
      for (auto& m : mobs) {
        if (!m.alive) continue;
        float hh = m.boss ? 1.5f : .6f;
        int g = groundBelow((int)m.p.x, (int)(m.p.y + 1.5f), (int)m.p.z);
        Vector3 flat = {body.x - m.p.x, 0, body.z - m.p.z};
        float fd = Vector3Length(flat);
        if (fd > 1.6f && fd < 45) {
          flat = Vector3Normalize(flat);
          float sp = (m.boss ? 2.1f : 1.3f) * dt;
          float mx = std::clamp(m.p.x + flat.x * sp, .5f, WX - .5f), mz = std::clamp(m.p.z + flat.z * sp, .5f, WZ - .5f);
          int ng = groundBelow((int)mx, (int)(m.p.y + 1.5f), (int)mz);
          if (ng - g <= 1) { m.p.x = mx; m.p.z = mz; g = ng; }  // can't climb walls taller than one block
        }
        m.p.y = g + hh;
        if (Vector3Distance(m.p, body) < 2.2f) {
          m.attack += dt;
          if (m.attack > 1.2f) { m.attack = 0; if (mode != Creative) health -= m.boss ? 14 : 5; }
        } else m.attack = 0;
      }
      mobs.erase(std::remove_if(mobs.begin(), mobs.end(), [](const Mob& m) { return !m.alive; }), mobs.end());
      if (bossSpawned && std::none_of(mobs.begin(), mobs.end(), [](const Mob& m) { return m.boss; })) bossSpawned = false;  // boss died: can be summoned again

      if (mode == Survival && health > 0 && health < 100) { regen += dt; if (regen > 2) { regen = 0; health++; } }
      if (health <= 0) {
        if (mode == Hardcore) { saveWorld(worldPath, mode); screen = 0; capture(false); status = "You fell in Hardcore. World saved."; continue; }
        health = 100; mobs.clear(); placePlayer(); status = "You died and respawned.";
      }
      day += dt * .035f;
    } else {
      // crafting overlay (the world is frozen while it is open)
      if (IsKeyPressed(KEY_ONE) && inv[4] >= 1) inv[4] += 3;  // 1 wood -> 4 planks (stored with wood)
      if (IsKeyPressed(KEY_TWO) && inv[2] >= 3 && inv[4] >= 2) { inv[2] -= 3; inv[4] -= 2; inv[10]++; }
      if (IsKeyPressed(KEY_THREE) && inv[8] >= 2 && inv[4] >= 1) { inv[8] -= 2; inv[4]--; inv[11]++; }
    }

    // ------------------------------------------------ network + chunk upkeep
    pollNet(net);
    netTimer += dt;
    if (net.connected && netTimer >= .05f) {
      netTimer = 0;
      StateMsg s{(uint8_t)PlayerState, net.id, cam.position.x, cam.position.y, cam.position.z, yaw, (uint8_t)std::clamp(health, 0, 255)};
      sendPacket(net, &s, sizeof(s), false);
    }
    rebuildDirty();

    // ------------------------------------------------ draw
    BeginDrawing();
    ClearBackground({(unsigned char)(105 + 25 * sinf(day)), (unsigned char)(165 + 20 * sinf(day)), (unsigned char)(210 + 15 * sinf(day)), 255});
    BeginMode3D(cam);
    DrawPlane({64, 0, 64}, {160, 160}, {74, 128, 70, 255});
    int ccx = std::clamp((int)feet.x, 0, WX - 1) / CS, ccz = std::clamp((int)feet.z, 0, WZ - 1) / CS;
    for (int x = std::max(0, ccx - renderDistance); x <= std::min(WX / CS - 1, ccx + renderDistance); x++)
      for (int z = std::max(0, ccz - renderDistance); z <= std::min(WZ / CS - 1, ccz + renderDistance); z++)
        if (chunks[x][z].ready) DrawModel(chunks[x][z].model, {0, 0, 0}, 1, WHITE);
    for (auto& m : mobs) if (m.alive) {
      DrawCube(m.p, m.boss ? 2.5f : 1.0f, m.boss ? 3.0f : 1.2f, m.boss ? 2.5f : 1.0f, m.boss ? PURPLE : MAROON);
      DrawCubeWires(m.p, m.boss ? 2.6f : 1.05f, m.boss ? 3.1f : 1.25f, m.boss ? 2.6f : 1.05f, BLACK);
    }
    for (int i = 0; i < net.count; i++) {
      Vector3 c = {net.others[i].pos.x, net.others[i].pos.y - .8f, net.others[i].pos.z};
      DrawCube(c, .6f, BODY_H, .6f, SKYBLUE);
      DrawCubeWires(c, .62f, BODY_H + .02f, .62f, DARKBLUE);
    }
    if (hx >= 0) DrawCubeWires({hx + .5f, hy + .5f, hz + .5f}, 1.01f, 1.01f, 1.01f, YELLOW);
    EndMode3D();

    const int W = GetScreenWidth(), H = GetScreenHeight();
    DrawRectangle(0, 0, W, 52, {7, 13, 24, 220});
    DrawText("BLOCKVERSE", 18, 12, 25, RAYWHITE);
    DrawText(mode == Survival ? "SURVIVAL" : mode == Creative ? "CREATIVE" : "HARDCORE", 210, 17, 16, SKYBLUE);
    DrawText(TextFormat("LV %d  XP %d/%d", level, xp, level * 100), W - 210, 16, 17, RAYWHITE);
    DrawRectangle(18, 66, 190, 100, {8, 14, 24, 190});
    DrawText(TextFormat("HP %d   HUNGER %d", std::max(0, health), hunger), 28, 77, 15, RAYWHITE);
    DrawText(TextFormat("STAMINA %d%%", (int)stamina), 28, 104, 15, GREEN);
    DrawText(TextFormat("PICKAXE x%d  BLADE x%d", inv[10], inv[11]), 28, 131, 15, GOLD);
    for (int i = 0; i < 9; i++) {
      int x = W / 2 - 225 + i * 50, y = H - 68;
      DrawRectangle(x, y, 46, 48, {13, 22, 34, 225});
      DrawRectangleLines(x, y, 46, 48, i == selected ? GOLD : GRAY);
      DrawRectangle(x + 11, y + 7, 24, 23, color(bar[i]));
      DrawText(TextFormat("%d", mode == Creative ? 999 : inv[i]), x + 27, y + 32, 10, RAYWHITE);
    }
    DrawText("+", W / 2 - 6, H / 2 - 14, 26, RAYWHITE);
    DrawText("WASD move | SPACE jump | SHIFT sprint | LMB mine/attack | RMB place | C craft | B boss | F5 save | TAB settings | ESC save+title", 18, H - 17, 12, RAYWHITE);
    if (net.connected) DrawText("ONLINE", W - 90, 65, 15, GREEN);
    if (showFps) DrawFPS(W - 90, 90);
    if (screen == 3) {
      DrawRectangle(0, 0, W, H, {0, 0, 0, 150});
      int px = W / 2 - 370, py = H / 2 - 215;
      DrawRectangle(px, py, 740, 430, {22, 34, 52, 255});
      DrawText("CRAFTING", px + 40, py + 30, 30, GOLD);
      DrawText(TextFormat("[1] 4 Planks   (1 Wood)             have: wood %d", inv[4]), px + 40, py + 100, 22, RAYWHITE);
      DrawText(TextFormat("[2] Stone Pickaxe   (3 Stone + 2 Wood)   stone %d", inv[2]), px + 40, py + 145, 22, RAYWHITE);
      DrawText(TextFormat("[3] Crystal Blade   (2 Crystal + 1 Wood)   crystal %d", inv[8]), px + 40, py + 190, 22, RAYWHITE);
      DrawText("Pickaxe: mined blocks drop double.  Blade: double damage.", px + 40, py + 260, 18, LIGHTGRAY);
      DrawText("Press C or ESC to close", px + 40, py + 330, 18, LIGHTGRAY);
    }
    EndDrawing();
  }

  if (worldLoaded) saveWorld(worldPath, mode);  // never overwrite a save with an empty world
  freeChunks();
  disconnectNet(net);
  if (enetUp) enet_deinitialize();
  persist();
  CloseWindow();
  return 0;
}
