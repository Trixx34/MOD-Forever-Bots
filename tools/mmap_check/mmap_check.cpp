// mmap_check: asks the navmesh files (mmaps) whether a dungeon route can be walked. A route point the mesh cannot reach was refused by the core as "no route"
// and stopped the leader (doc/dungeons.md). This uses the same Detour library and the same file layout as the worldserver, but not its movement code, so it
// tells which points are off the mesh or cut off from the previous point, not exactly where the worldserver's pathfinder would give up.
//
//   mmap_check <mmaps folder> <dungeons.txt> <dungeon id>            check the route, one line per leg
//   mmap_check <mmaps folder> <dungeons.txt> <dungeon id> --fix      print the dungeon block again with the points that cannot be reached dropped and the rest
//                                                                     ordered by walking distance (nearest next), bosses kept
//   mmap_check <mmaps folder> <dungeons.txt> <dungeon id> --plan     print the dungeon block again with a new route: boss to boss over the navmesh. The bosses
//                                                                     are visited nearest first by walking distance from the entry; between two bosses the
//                                                                     route is the corners of the walkable path, so it follows the corridors and every point
//                                                                     is on the mesh by construction. Trash is left to the party (it fights what it meets).
// Build: tools/mmap_check/build.sh
#include <DetourCommon.h>
#include <DetourNavMesh.h>
#include <DetourNavMeshQuery.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct NavMeshHeader
{
    std::uint32_t magic;
    std::uint32_t version;
    dtNavMeshParams params;
    std::uint32_t offmeshConnectionCount;
};
static_assert(sizeof(NavMeshHeader) == 40, "the .mmap header is 40 bytes");

struct TileHeader
{
    std::uint32_t magic;
    std::uint32_t dtVersion;
    std::uint32_t mmapVersion;
    std::uint32_t size;
    char usesLiquids;
    char padding[3];
};
static_assert(sizeof(TileHeader) == 20, "the .mmtile header is 20 bytes");

constexpr std::uint32_t kMagic = 0x4d4d4150;

struct Point
{
    std::string Kind;   // go or boss
    std::string Line;   // the text of the line, for printing again
    float X = 0, Y = 0, Z = 0;
    std::string Name;
};

struct Dungeon
{
    std::vector<std::string> Header;   // every line before the first go / boss
    std::vector<std::string> Tail;     // the lines after the last one ("end")
    float EntryX = 0, EntryY = 0, EntryZ = 0;
    std::uint32_t Map = 0;
    std::vector<Point> Route;
};

bool Load(std::string const& file, std::string const& id, Dungeon& out)
{
    std::ifstream in(file);
    if (!in)
        return false;
    std::string line;
    bool inside = false;
    bool afterRoute = false;
    while (std::getline(in, line))
    {
        std::istringstream words(line);
        std::string head;
        words >> head;
        if (!inside)
        {
            if (head == "dungeon")
            {
                std::string name;
                words >> name;
                inside = name == id;
                if (inside)
                    out.Header.push_back(line);
            }
            continue;
        }
        if (head == "go" || head == "boss")
        {
            Point point;
            point.Kind = head;
            point.Line = line;
            if (head == "go")
                words >> point.X >> point.Y >> point.Z;
            else
            {
                std::string entry;
                words >> entry >> point.X >> point.Y >> point.Z;
                std::getline(words, point.Name);
            }
            out.Route.push_back(point);
            afterRoute = true;
        }
        else if (head == "end")
        {
            out.Tail.push_back(line);
            return true;
        }
        else
        {
            if (head == "entry")
                words >> out.Map >> out.EntryX >> out.EntryY >> out.EntryZ;
            (afterRoute ? out.Tail : out.Header).push_back(line);
        }
    }
    return inside;
}

struct Mesh
{
    dtNavMesh* Nav = nullptr;
    dtNavMeshQuery* Query = nullptr;
    dtQueryFilter Filter;
    unsigned Tiles = 0;

    bool Open(std::string const& folder, std::uint32_t map)
    {
        char name[32];
        std::snprintf(name, sizeof(name), "%04u.mmap", map);
        std::ifstream header(folder + "/" + name, std::ios::binary);
        NavMeshHeader navHeader{};
        if (!header.read(reinterpret_cast<char*>(&navHeader), sizeof(navHeader)) || navHeader.magic != kMagic)
        {
            std::fprintf(stderr, "cannot read %s/%s\n", folder.c_str(), name);
            return false;
        }
        Nav = dtAllocNavMesh();
        if (dtStatusFailed(Nav->init(&navHeader.params)))
            return false;

        char prefix[16];
        std::snprintf(prefix, sizeof(prefix), "%04u_", map);
        if (DIR* dir = opendir(folder.c_str()))
        {
            while (dirent* entry = readdir(dir))
            {
                std::string const file = entry->d_name;
                if (file.rfind(prefix, 0) != 0 || file.size() < 7 || file.substr(file.size() - 7) != ".mmtile")
                    continue;
                std::ifstream tile(folder + "/" + file, std::ios::binary);
                TileHeader tileHeader{};
                if (!tile.read(reinterpret_cast<char*>(&tileHeader), sizeof(tileHeader)) || tileHeader.magic != kMagic || tileHeader.size == 0)
                    continue;
                unsigned char* data = static_cast<unsigned char*>(dtAlloc(tileHeader.size, DT_ALLOC_PERM));
                if (!tile.read(reinterpret_cast<char*>(data), tileHeader.size))
                {
                    dtFree(data);
                    continue;
                }
                if (dtStatusFailed(Nav->addTile(data, int(tileHeader.size), DT_TILE_FREE_DATA, 0, nullptr)))
                    dtFree(data);
                else
                    ++Tiles;
            }
            closedir(dir);
        }
        Query = dtAllocNavMeshQuery();
        if (dtStatusFailed(Query->init(Nav, 2048)))
            return false;
        Filter.setIncludeFlags(0x01 | 0x02 | 0x04);   // ground, steep ground, water: not magma or slime
        Filter.setExcludeFlags(0);
        return true;
    }
};

// World (x, y, z) to the Detour axes the worldserver uses: (y, z, x).
void ToDetour(float x, float y, float z, float out[3])
{
    out[0] = y;
    out[1] = z;
    out[2] = x;
}

enum class Leg { Ok, StartOffMesh, EndOffMesh, Cut, Partial };

struct Result
{
    Leg Kind = Leg::Ok;
    float Length = 0;
};

dtPolyRef NearestPoly(Mesh& mesh, float const world[3], float snapped[3])
{
    float pos[3];
    ToDetour(world[0], world[1], world[2], pos);
    float const extents[3] = { 3.0f, 5.0f, 3.0f };
    dtPolyRef ref = 0;
    mesh.Query->findNearestPoly(pos, extents, &mesh.Filter, &ref, snapped);
    return ref;
}

Result Walk(Mesh& mesh, float const from[3], float const to[3])
{
    Result result;
    float startPos[3], endPos[3];
    dtPolyRef const start = NearestPoly(mesh, from, startPos);
    dtPolyRef const end = NearestPoly(mesh, to, endPos);
    if (!start)
    {
        result.Kind = Leg::StartOffMesh;
        return result;
    }
    if (!end)
    {
        result.Kind = Leg::EndOffMesh;
        return result;
    }
    dtPolyRef polys[256];
    int count = 0;
    dtStatus const status = mesh.Query->findPath(start, end, startPos, endPos, &mesh.Filter, polys, &count, 256);
    if (dtStatusFailed(status) || count == 0)
    {
        result.Kind = Leg::Cut;
        return result;
    }
    if (polys[count - 1] != end)
        result.Kind = Leg::Partial;
    float straight[3 * 256];
    unsigned char flags[256];
    dtPolyRef straightPolys[256];
    int points = 0;
    float target[3] = { endPos[0], endPos[1], endPos[2] };
    if (result.Kind == Leg::Partial)
        mesh.Query->closestPointOnPoly(polys[count - 1], endPos, target, nullptr);
    mesh.Query->findStraightPath(startPos, target, polys, count, straight, flags, straightPolys, &points, 256);
    for (int i = 1; i < points; ++i)
        result.Length += dtVdist(&straight[3 * (i - 1)], &straight[3 * i]);
    return result;
}

char const* Name(Leg kind)
{
    switch (kind)
    {
        case Leg::Ok: return "ok";
        case Leg::StartOffMesh: return "start is off the mesh";
        case Leg::EndOffMesh: return "point is off the mesh";
        case Leg::Cut: return "no path";
        case Leg::Partial: return "partial path (the mesh ends short of the point)";
    }
    return "?";
}

// The corners of the walkable path between two places, as world coordinates (the start is not included). Empty when there is no path.
std::vector<Point> Corners(Mesh& mesh, float const from[3], float const to[3], bool& partial)
{
    std::vector<Point> out;
    partial = false;
    float startPos[3], endPos[3];
    dtPolyRef const start = NearestPoly(mesh, from, startPos);
    dtPolyRef const end = NearestPoly(mesh, to, endPos);
    if (!start || !end)
        return out;
    dtPolyRef polys[512];
    int count = 0;
    if (dtStatusFailed(mesh.Query->findPath(start, end, startPos, endPos, &mesh.Filter, polys, &count, 512)) || count == 0)
        return out;
    float target[3] = { endPos[0], endPos[1], endPos[2] };
    if (polys[count - 1] != end)
    {
        partial = true;
        mesh.Query->closestPointOnPoly(polys[count - 1], endPos, target, nullptr);
    }
    float straight[3 * 512];
    unsigned char flags[512];
    dtPolyRef straightPolys[512];
    int points = 0;
    mesh.Query->findStraightPath(startPos, target, polys, count, straight, flags, straightPolys, &points, 512);
    for (int i = 1; i < points; ++i)
    {
        Point point;
        point.Kind = "go";
        point.X = straight[3 * i + 2];   // back from Detour (y, z, x) to the world (x, y, z)
        point.Y = straight[3 * i + 0];
        point.Z = straight[3 * i + 1];
        out.push_back(point);
    }
    return out;
}

std::string Fixed(float value)
{
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.1f", value);
    return buffer;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 4)
    {
        std::fprintf(stderr, "usage: mmap_check <mmaps folder> <dungeons.txt> <dungeon id> [--fix]\n");
        return 2;
    }
    bool const fix = argc > 4 && std::string(argv[4]) == "--fix";
    bool const plan = argc > 4 && std::string(argv[4]) == "--plan";
    if (argc > 4 && std::string(argv[4]) == "--ring" && argc >= 10)
    {
        // mmap_check <mmaps> <dungeons.txt> <id> --ring <map> <x> <y> <z> <radius> : walkable points on a ring of that radius around a place that the place
        // at <from x y z> can reach by walking: [map x y z] is taken from argv[5..8], the radius is argv[9], the start is argv[10..12]. Prints the points, nearest to the start first.
        Mesh mesh;
        std::uint32_t const map = std::uint32_t(std::atoi(argv[5]));
        if (!mesh.Open(argv[1], map))
            return 2;
        float const centre[3] = { float(std::atof(argv[6])), float(std::atof(argv[7])), float(std::atof(argv[8])) };
        float const radius = float(std::atof(argv[9]));
        float const start[3] = { float(std::atof(argv[10])), float(std::atof(argv[11])), float(std::atof(argv[12])) };
        float startPos[3];
        dtPolyRef const startRef = NearestPoly(mesh, start, startPos);
        if (!startRef)
        {
            std::printf("the start is off the mesh\n");
            return 1;
        }
        std::vector<std::pair<float, std::array<float, 3>>> found;
        for (int a = 0; a < 360; a += 10)
        {
            float const rad = float(a) * 3.14159265f / 180.0f;
            float const probe[3] = { centre[0] + radius * std::cos(rad), centre[1] + radius * std::sin(rad), centre[2] };
            float snapped[3];
            if (!NearestPoly(mesh, probe, snapped))
                continue;
            float const onRing = std::hypot(snapped[2] - centre[0], snapped[0] - centre[1]);
            Result const result = Walk(mesh, start, snapped);
            if (result.Kind != Leg::Ok)
                continue;
            found.push_back({ result.Length, { snapped[2], snapped[0], snapped[1] } });
            std::printf("%.1f %.1f %.1f  ring=%.1f walk=%.0f\n", snapped[2], snapped[0], snapped[1], onRing, result.Length);
        }
        if (found.empty())
            std::printf("no point on the ring is walkable from the start\n");
        return 0;
    }
    if (argc > 4 && std::string(argv[4]) == "--near" && argc >= 8)
    {
        // mmap_check <mmaps folder> <dungeons.txt> <dungeon id> --near <map> <x> <y> <z>: the walkable point nearest to a place (for doors and levers, which stand off the mesh)
        Mesh mesh;
        std::uint32_t const map = std::uint32_t(std::atoi(argv[5]));
        if (!mesh.Open(argv[1], map))
            return 2;
        float world[3] = { float(std::atof(argv[6])), float(std::atof(argv[7])), float(std::atof(argv[8])) };
        float pos[3];
        float detour[3];
        ToDetour(world[0], world[1], world[2], detour);
        float const extents[3] = { 8.0f, 8.0f, 8.0f };
        dtPolyRef ref = 0;
        mesh.Query->findNearestPoly(detour, extents, &mesh.Filter, &ref, pos);
        if (!ref)
        {
            std::printf("none within 8 yards\n");
            return 1;
        }
        std::printf("%.2f %.2f %.2f  (%.2f yards away)\n", pos[2], pos[0], pos[1],
                    std::hypot(pos[2] - world[0], pos[0] - world[1]));
        return 0;
    }
    Dungeon dungeon;
    if (!Load(argv[2], argv[3], dungeon) || dungeon.Route.empty() || dungeon.Map == 0)
    {
        std::fprintf(stderr, "dungeon %s not found or has no entry/route in %s\n", argv[3], argv[2]);
        return 2;
    }
    Mesh mesh;
    if (!mesh.Open(argv[1], dungeon.Map))
        return 2;
    std::fprintf(stderr, "map %u: %u tiles\n", dungeon.Map, mesh.Tiles);

    float entry[3] = { dungeon.EntryX, dungeon.EntryY, dungeon.EntryZ };
    float snapped[3];
    if (!NearestPoly(mesh, entry, snapped))
        std::fprintf(stderr, "WARNING: the entry point itself is off the mesh\n");

    if (plan)
    {
        constexpr float kMinCornerGap = 10.0f;   // corners closer than this to the one before are dropped (the final point of a leg is always kept)
        std::vector<Point> bosses;
        for (Point const& point : dungeon.Route)
            if (point.Kind == "boss")
                bosses.push_back(point);
        float at[3] = { dungeon.EntryX, dungeon.EntryY, dungeon.EntryZ };
        std::vector<Point> route;
        int unreachable = 0;
        while (!bosses.empty())
        {
            std::size_t best = bosses.size();
            float bestLength = 1e30f;
            for (std::size_t i = 0; i < bosses.size(); ++i)
            {
                float here[3] = { bosses[i].X, bosses[i].Y, bosses[i].Z };
                Result const result = Walk(mesh, at, here);
                if (result.Kind == Leg::Ok && result.Length < bestLength)
                {
                    bestLength = result.Length;
                    best = i;
                }
            }
            if (best == bosses.size())
            {
                for (Point const& lost : bosses)
                    std::fprintf(stderr, "boss %s cannot be reached from the last boss: left out\n", lost.Name.c_str());
                unreachable = int(bosses.size());
                break;
            }
            float here[3] = { bosses[best].X, bosses[best].Y, bosses[best].Z };
            bool partial = false;
            std::vector<Point> corners = Corners(mesh, at, here, partial);
            float last[2] = { at[0], at[1] };
            for (std::size_t i = 0; i < corners.size(); ++i)
            {
                bool const final = i + 1 == corners.size();
                if (!final && std::hypot(corners[i].X - last[0], corners[i].Y - last[1]) < kMinCornerGap)
                    continue;
                if (final)
                    break;   // the boss itself is the last point of the leg
                route.push_back(corners[i]);
                last[0] = corners[i].X;
                last[1] = corners[i].Y;
            }
            route.push_back(bosses[best]);
            at[0] = bosses[best].X;
            at[1] = bosses[best].Y;
            at[2] = bosses[best].Z;
            bosses.erase(bosses.begin() + std::ptrdiff_t(best));
        }
        std::fprintf(stderr, "planned %zu points for %zu legs; %d boss(es) left out\n", route.size(), dungeon.Route.size() - std::size_t(unreachable), unreachable);
        for (std::string const& line : dungeon.Header)
            std::printf("%s\n", line.c_str());
        for (Point const& point : route)
        {
            if (point.Kind == "boss")
                std::printf("%s\n", point.Line.c_str());
            else
                std::printf("go %s %s %s\n", Fixed(point.X).c_str(), Fixed(point.Y).c_str(), Fixed(point.Z).c_str());
        }
        for (std::string const& line : dungeon.Tail)
            std::printf("%s\n", line.c_str());
        return 0;
    }

    if (!fix)
    {
        float previous[3] = { dungeon.EntryX, dungeon.EntryY, dungeon.EntryZ };
        int bad = 0;
        float total = 0;
        for (std::size_t i = 0; i < dungeon.Route.size(); ++i)
        {
            Point const& point = dungeon.Route[i];
            float here[3] = { point.X, point.Y, point.Z };
            Result const result = Walk(mesh, previous, here);
            std::printf("%3zu %-4s %8.1f %8.1f %7.1f  %-8s %s%s\n", i, point.Kind.c_str(), point.X, point.Y, point.Z, result.Kind == Leg::Ok ? "ok" : "BAD",
                        Name(result.Kind), point.Name.c_str());
            if (result.Kind == Leg::Ok || result.Kind == Leg::Partial)
            {
                total += result.Length;
                std::memcpy(previous, here, sizeof(previous));
            }
            bad += result.Kind != Leg::Ok;
        }
        std::printf("%zu points, %d not reachable from the one before, %.0f yards of path\n", dungeon.Route.size(), bad, total);
        return bad ? 1 : 0;
    }

    // --fix: drop what is not on the mesh or not connected to the entry, then order the rest nearest-next by walking distance. A boss stays if it can be reached.
    std::vector<Point> left;
    int dropped = 0;
    for (Point const& point : dungeon.Route)
    {
        float here[3] = { point.X, point.Y, point.Z };
        float entryPos[3] = { dungeon.EntryX, dungeon.EntryY, dungeon.EntryZ };
        Result const fromEntry = Walk(mesh, entryPos, here);
        if (fromEntry.Kind == Leg::Ok)
            left.push_back(point);
        else
        {
            ++dropped;
            std::fprintf(stderr, "dropped %s %.1f %.1f %.1f %s: %s\n", point.Kind.c_str(), point.X, point.Y, point.Z, point.Name.c_str(), Name(fromEntry.Kind));
        }
    }
    std::vector<Point> ordered;
    float at[3] = { dungeon.EntryX, dungeon.EntryY, dungeon.EntryZ };
    while (!left.empty())
    {
        std::size_t best = 0;
        float bestLength = 1e30f;
        for (std::size_t i = 0; i < left.size(); ++i)
        {
            float here[3] = { left[i].X, left[i].Y, left[i].Z };
            Result const result = Walk(mesh, at, here);
            if (result.Kind == Leg::Ok && result.Length < bestLength)
            {
                bestLength = result.Length;
                best = i;
            }
        }
        if (bestLength > 1e29f)
            break; // the rest cannot be reached from here
        ordered.push_back(left[best]);
        at[0] = left[best].X;
        at[1] = left[best].Y;
        at[2] = left[best].Z;
        left.erase(left.begin() + std::ptrdiff_t(best));
    }
    std::fprintf(stderr, "%zu points kept, %d dropped (not reachable from the entry), %zu left unordered\n", ordered.size(), dropped, left.size());
    for (std::string const& line : dungeon.Header)
        std::printf("%s\n", line.c_str());
    for (Point const& point : ordered)
        std::printf("%s\n", point.Line.c_str());
    for (std::string const& line : dungeon.Tail)
        std::printf("%s\n", line.c_str());
    return 0;
}
