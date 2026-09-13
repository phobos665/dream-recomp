// Display-list decoding (WP2.3 step 2). The streams here are built by hand from the hardware's
// parameter formats, so a failure points at the decoder rather than at a capture. A real Crazy
// Taxi frame is replayed too when one has been captured (see the launcher's --dump-ta).
#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <vector>

#include "dream/render/display_list.h"
#include "dream/runtime/pvr/core.h"

#include "doctest.h"

namespace {

using namespace dream::render;
using u32 = std::uint32_t;

u32 f2u(float f) {
    u32 w;
    std::memcpy(&w, &f, 4);
    return w;
}

// Parameter control word. Defaults describe an opaque, untextured, packed-colour polygon.
struct Pcw {
    u32 para_type = 4;  // polygon or modifier volume
    u32 list = 0;       // opaque
    u32 end_of_strip = 0;
    u32 col_type = 0;  // packed
    u32 texture = 0;
    u32 offset = 0;
    u32 uv16 = 0;
    u32 volume = 0;
    u32 shadow = 0;
    u32 word() const {
        return (para_type << 29) | (end_of_strip << 28) | (list << 24) | (shadow << 7) |
               (volume << 6) | (col_type << 4) | (texture << 3) | (offset << 2) | uv16;
    }
};

struct Stream {
    std::vector<u32> words;
    void chunk(std::initializer_list<u32> w) {
        std::array<u32, 8> c{};
        std::size_t i = 0;
        for (u32 x : w) c[i++] = x;
        for (u32 x : c) words.push_back(x);
    }
    void feed(DisplayList& dl) const { dl.feed_stream(words.data(), words.size()); }
};

// A packed-colour, untextured vertex: xyz then the colour at word 6.
void packed_vertex(Stream& s, float x, float y, float z, u32 colour, bool end) {
    Pcw p;
    p.para_type = 7;
    p.end_of_strip = end ? 1u : 0u;
    s.chunk({p.word(), f2u(x), f2u(y), f2u(z), 0, 0, colour, 0});
}

}  // namespace

TEST_CASE("display list: a packed-colour triangle strip becomes one polygon") {
    Stream s;
    Pcw header;
    s.chunk({header.word(), 0x01234567u, 0x89ABCDEFu, 0u});  // header: isp, tsp, tcw
    packed_vertex(s, 10, 20, 0.5f, 0xFF112233u, false);
    packed_vertex(s, 30, 20, 0.5f, 0xFF445566u, false);
    packed_vertex(s, 20, 40, 0.25f, 0xFF778899u, true);
    Pcw eol;
    eol.para_type = 0;
    s.chunk({eol.word()});

    DisplayList dl;
    s.feed(dl);
    const Frame& f = dl.frame();

    REQUIRE(f.lists[0].size() == 1);
    const Polygon& p = f.lists[0][0];
    CHECK(p.count == 3);
    CHECK(p.first == 0);
    CHECK(p.isp == 0x01234567u);
    CHECK(p.tsp == 0x89ABCDEFu);
    REQUIRE(f.vertices.size() == 3);
    CHECK(f.vertices[0].x == doctest::Approx(10));
    CHECK(f.vertices[0].y == doctest::Approx(20));
    CHECK(f.vertices[0].z == doctest::Approx(0.5f));
    CHECK(f.vertices[0].base == 0xFF112233u);
    CHECK(f.vertices[2].base == 0xFF778899u);
    CHECK(f.vertices[2].z == doctest::Approx(0.25f));
    CHECK(f.min_z == doctest::Approx(0.25f));
    CHECK(f.max_z == doctest::Approx(0.5f));
    CHECK(f.strips == 1);
    CHECK(f.dropped_vertices == 0);
    CHECK(f.unknown_parameters == 0);
}

TEST_CASE("display list: strips are split at end-of-strip and lists are kept apart") {
    Stream s;
    Pcw opaque;
    s.chunk({opaque.word(), 0, 0, 0});
    packed_vertex(s, 0, 0, 1, 0xFFFFFFFFu, false);
    packed_vertex(s, 1, 0, 1, 0xFFFFFFFFu, false);
    packed_vertex(s, 0, 1, 1, 0xFFFFFFFFu, true);
    packed_vertex(s, 2, 2, 1, 0xFFFFFFFFu, false);  // a second strip under the same header
    packed_vertex(s, 3, 2, 1, 0xFFFFFFFFu, false);
    packed_vertex(s, 2, 3, 1, 0xFFFFFFFFu, false);
    packed_vertex(s, 3, 3, 1, 0xFFFFFFFFu, true);
    Pcw eol;
    eol.para_type = 0;
    s.chunk({eol.word()});

    Pcw translucent;
    translucent.list = 2;
    s.chunk({translucent.word(), 0, 0, 0});
    packed_vertex(s, 5, 5, 1, 0xFF000000u, false);
    packed_vertex(s, 6, 5, 1, 0xFF000000u, false);
    packed_vertex(s, 5, 6, 1, 0xFF000000u, true);
    s.chunk({eol.word()});

    DisplayList dl;
    s.feed(dl);
    const Frame& f = dl.frame();
    REQUIRE(f.lists[0].size() == 2);
    CHECK(f.lists[0][0].count == 3);
    CHECK(f.lists[0][1].count == 4);
    CHECK(f.lists[0][1].first == 3);
    REQUIRE(f.lists[2].size() == 1);
    CHECK(f.lists[2][0].count == 3);
    CHECK(f.polygon_count() == 3);
    CHECK(f.vertices.size() == 10);
}

TEST_CASE("display list: a strip of fewer than three vertices is discarded") {
    Stream s;
    Pcw header;
    s.chunk({header.word(), 0, 0, 0});
    packed_vertex(s, 0, 0, 1, 0xFFFFFFFFu, false);
    packed_vertex(s, 1, 0, 1, 0xFFFFFFFFu, true);  // only two: not a triangle
    Pcw eol;
    eol.para_type = 0;
    s.chunk({eol.word()});

    DisplayList dl;
    s.feed(dl);
    CHECK(dl.frame().polygon_count() == 0);
    CHECK(dl.frame().strips == 0);
}

TEST_CASE("display list: textured vertices carry their coordinates, 32-bit and 16-bit") {
    // 32-bit coordinates
    {
        Stream s;
        Pcw header;
        header.texture = 1;
        s.chunk({header.word(), 0, 0, 0x0Du});
        for (int i = 0; i < 3; ++i) {
            Pcw v;
            v.para_type = 7;
            v.end_of_strip = i == 2 ? 1u : 0u;
            s.chunk({v.word(), f2u(static_cast<float>(i)), f2u(0), f2u(1),
                     f2u(0.25f * static_cast<float>(i + 1)), f2u(0.5f), 0xFF808080u, 0});
        }
        DisplayList dl;
        s.feed(dl);
        const Frame& f = dl.frame();
        REQUIRE(f.vertices.size() == 3);
        CHECK(f.vertices[0].u == doctest::Approx(0.25f));
        CHECK(f.vertices[0].v == doctest::Approx(0.5f));
        CHECK(f.vertices[2].u == doctest::Approx(0.75f));
        CHECK(f.lists[0][0].tcw == 0x0Du);
    }
    // 16-bit coordinates: the top half of each float, v in the low half of the word
    {
        Stream s;
        Pcw header;
        header.texture = 1;
        header.uv16 = 1;
        s.chunk({header.word(), 0, 0, 0});
        const u32 u_half = f2u(0.25f) >> 16, v_half = f2u(0.5f) >> 16;
        for (int i = 0; i < 3; ++i) {
            Pcw v;
            v.para_type = 7;
            v.uv16 = 1;
            v.end_of_strip = i == 2 ? 1u : 0u;
            s.chunk({v.word(), f2u(static_cast<float>(i)), f2u(0), f2u(1), (u_half << 16) | v_half,
                     0, 0xFF808080u, 0});
        }
        DisplayList dl;
        s.feed(dl);
        const Frame& f = dl.frame();
        REQUIRE(f.vertices.size() == 3);
        CHECK(f.vertices[0].u == doctest::Approx(0.25f));
        CHECK(f.vertices[0].v == doctest::Approx(0.5f));
    }
}

// Three intensity-mode vertices of a strip: x = i, intensity full on the first and half after.
static void intensity_strip(Stream& s) {
    for (int i = 0; i < 3; ++i) {
        Pcw v;
        v.para_type = 7;
        v.col_type = 2;
        v.texture = 1;
        v.end_of_strip = i == 2 ? 1u : 0u;
        // xyz, u, v, then the intensity at word 6.
        s.chunk({v.word(), f2u(static_cast<float>(i)), f2u(0), f2u(1), f2u(0), f2u(0),
                 f2u(i == 0 ? 1.0f : 0.5f), 0});
    }
}

TEST_CASE("display list: a 64-byte intensity header carries its face colour in the second half") {
    // Intensity mode 1 with both a texture and an offset colour: polygon type 2, 64 bytes, because
    // two colours do not fit in the first chunk.
    Stream s;
    Pcw header;
    header.col_type = 2;
    header.texture = 1;
    header.offset = 1;
    s.chunk({header.word(), 0, 0, 0});
    s.chunk({f2u(1.0f), f2u(1.0f), f2u(0.5f), f2u(0.0f), f2u(0), f2u(0), f2u(0), f2u(0)});
    intensity_strip(s);

    DisplayList dl;
    s.feed(dl);
    const Frame& f = dl.frame();
    REQUIRE(f.vertices.size() == 3);
    // Full intensity reproduces the face colour; half intensity halves the RGB and keeps alpha.
    CHECK(f.vertices[0].base == 0xFFFF8000u);
    CHECK(((f.vertices[1].base >> 24) & 0xFFu) == 0xFFu);
    CHECK(((f.vertices[1].base >> 16) & 0xFFu) == 0x80u);
    CHECK(((f.vertices[1].base >> 8) & 0xFFu) == 0x40u);
}

TEST_CASE("display list: a 32-byte intensity header carries its face colour in words 4 to 7") {
    // The same colour mode without an offset colour: polygon type 1, 32 bytes, and the face colour
    // sits in the words a header with no face colour leaves unused. Reading this header as 64
    // bytes swallows the first vertex of the strip, and treating it as having no face colour
    // washes the polygon out to white.
    Stream s;
    Pcw header;
    header.col_type = 2;
    header.texture = 1;  // no offset colour
    s.chunk({header.word(), 0, 0, 0, f2u(1.0f), f2u(1.0f), f2u(0.5f), f2u(0.0f)});
    intensity_strip(s);

    DisplayList dl;
    s.feed(dl);
    const Frame& f = dl.frame();
    REQUIRE(f.vertices.size() == 3);  // no vertex eaten by a mis-sized header
    CHECK(f.vertices[0].base == 0xFFFF8000u);
    CHECK(((f.vertices[1].base >> 16) & 0xFFu) == 0x80u);
}

TEST_CASE("display list: intensity mode 2 keeps the previous face colour and a 32-byte header") {
    // Mode 2 reuses the face colour the last polygon established, so it never carries one however
    // it is textured. Crazy Taxi's SEGA logo mixes the two modes in one screen: reading a mode 2
    // header as 64 bytes cost each of those strips a vertex and drew wedges across the letters.
    Stream s;
    Pcw first;
    first.col_type = 2;
    first.texture = 1;
    s.chunk({first.word(), 0, 0, 0, f2u(1.0f), f2u(1.0f), f2u(0.5f), f2u(0.0f)});
    intensity_strip(s);

    Pcw second;
    second.col_type = 3;  // reuse the face colour
    second.texture = 1;
    second.offset = 1;  // would have made mode 1 a 64-byte header; mode 2 stays 32
    s.chunk({second.word(), 0, 0, 0});
    intensity_strip(s);

    DisplayList dl;
    s.feed(dl);
    const Frame& f = dl.frame();
    REQUIRE(f.vertices.size() == 6);  // three per strip, none eaten
    REQUIRE(f.lists[0].size() == 2);
    CHECK(f.lists[0][1].count == 3);
    CHECK(f.vertices[3].base == 0xFFFF8000u);  // the first polygon's face colour, still in force
}

TEST_CASE("display list: floating-point colours come from the vertex's second half") {
    Stream s;
    Pcw header;
    header.col_type = 1;  // floating
    header.texture = 1;   // textured floating colour makes the vertex 64 bytes
    s.chunk({header.word(), 0, 0, 0});
    for (int i = 0; i < 3; ++i) {
        Pcw v;
        v.para_type = 7;
        v.col_type = 1;
        v.texture = 1;
        v.end_of_strip = i == 2 ? 1u : 0u;
        s.chunk({v.word(), f2u(static_cast<float>(i)), f2u(0), f2u(1), f2u(0), f2u(0), 0, 0});
        s.chunk({f2u(1.0f), f2u(0.0f), f2u(1.0f), f2u(0.0f), 0, 0, 0, 0});  // A R G B
    }
    DisplayList dl;
    s.feed(dl);
    const Frame& f = dl.frame();
    REQUIRE(f.vertices.size() == 3);
    CHECK(f.vertices[0].base == 0xFF00FF00u);
}

TEST_CASE("display list: a sprite becomes a four-vertex quad with the fourth corner solved") {
    Stream s;
    Pcw header;
    header.para_type = 5;  // sprite
    // Header: isp, tsp, tcw, base colour, offset colour.
    s.chunk({header.word(), 0, 0, 0, 0xFF203040u, 0});
    // Sprite vertex: x0 y0 z0 x1 y1 z1 x2 | y2 z2 x3 y3 ...
    // A unit square in the z = 2 plane; the fourth corner must come out at z = 2 as well.
    Pcw v;
    v.para_type = 7;
    s.chunk({v.word(), f2u(0), f2u(0), f2u(2), f2u(10), f2u(0), f2u(2), f2u(10)});
    s.chunk({f2u(10), f2u(2), f2u(0), f2u(10), 0, 0, 0, 0});

    DisplayList dl;
    s.feed(dl);
    const Frame& f = dl.frame();
    REQUIRE(f.lists[0].size() == 1);
    CHECK(f.lists[0][0].count == 4);
    REQUIRE(f.vertices.size() == 4);
    CHECK(f.sprites == 1);
    for (const Vertex& vert : f.vertices) {
        CHECK(vert.base == 0xFF203040u);
        CHECK(vert.z == doctest::Approx(2.0f));
    }
}

TEST_CASE("display list: modifier volumes decode to triangles, not strips") {
    Stream s;
    Pcw header;
    header.list = 1;  // opaque modifier volume
    s.chunk({header.word(), 0xDEADBEEFu, 0, 0});
    Pcw v;
    v.para_type = 7;
    v.list = 1;
    // One triangle spans both halves: x0 y0 z0 x1 y1 z1 x2 | y2 z2
    s.chunk({v.word(), f2u(0), f2u(0), f2u(1), f2u(1), f2u(0), f2u(1), f2u(0)});
    s.chunk({f2u(1), f2u(1), 0, 0, 0, 0, 0, 0});

    DisplayList dl;
    s.feed(dl);
    const Frame& f = dl.frame();
    REQUIRE(f.modifiers.size() == 1);
    CHECK(f.modifiers[0].isp == 0xDEADBEEFu);
    CHECK(f.modifiers[0].x[0] == doctest::Approx(0));
    CHECK(f.modifiers[0].x[1] == doctest::Approx(1));
    CHECK(f.modifiers[0].y[2] == doctest::Approx(1));
    CHECK(f.modifiers[0].z[2] == doctest::Approx(1));
    CHECK(f.polygon_count() == 0);  // modifier volumes are not drawable polygons
}

TEST_CASE("display list: a vertex with no header is counted, not crashed on") {
    Stream s;
    packed_vertex(s, 0, 0, 1, 0xFFFFFFFFu, true);
    DisplayList dl;
    s.feed(dl);
    CHECK(dl.frame().dropped_vertices == 1);
    CHECK(dl.frame().vertices.empty());
}

TEST_CASE("display list: a captured Crazy Taxi frame decodes without errors") {
    // Written by `crazytaxi_boot --dump-ta FILE`; owner-supplied game data, absent on CI.
    const char* path = std::getenv("DREAM_TA_CAPTURE");
    std::ifstream in(path ? path : "games/crazytaxi/extracted/ta_frame.bin", std::ios::binary);
    if (!in) {
        MESSAGE("no captured TA frame; skipping");
        return;
    }
    std::vector<u32> words((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.clear();
    in.seekg(0, std::ios::end);
    const std::size_t bytes = static_cast<std::size_t>(in.tellg());
    in.seekg(0);
    words.assign(bytes / 4, 0);
    in.read(reinterpret_cast<char*>(words.data()), static_cast<std::streamsize>(bytes));

    DisplayList dl;
    dl.feed_stream(words.data(), words.size());
    const Frame& f = dl.frame();
    MESSAGE("captured frame: " << f.polygon_count() << " polygons, " << f.vertices.size()
                               << " vertices, " << f.sprites << " sprites, " << f.modifiers.size()
                               << " modifier triangles");
    CHECK(f.unknown_parameters == 0);
    CHECK(f.dropped_vertices == 0);
    CHECK(f.polygon_count() > 0);
    for (const auto& list : f.lists)
        for (const Polygon& p : list) {
            CHECK(p.count >= 3);
            CHECK(p.first + p.count <= f.vertices.size());
        }
}

// The runtime has its own parameter parser, written for timing and statistics and independent of
// this decoder. Feeding both the same stream and comparing what they counted catches a
// misunderstanding of the 32/64-byte rules in either one.
TEST_CASE("display list: agrees with the runtime's independent parser on a captured frame") {
    const char* path = std::getenv("DREAM_TA_CAPTURE");
    std::ifstream in(path ? path : "games/crazytaxi/extracted/ta_frame.bin", std::ios::binary);
    if (!in) {
        MESSAGE("no captured TA frame; skipping");
        return;
    }
    in.seekg(0, std::ios::end);
    const std::size_t bytes = static_cast<std::size_t>(in.tellg());
    in.seekg(0);
    std::vector<u32> words(bytes / 4, 0);
    in.read(reinterpret_cast<char*>(words.data()), static_cast<std::streamsize>(bytes));

    DisplayList dl;
    dl.feed_stream(words.data(), words.size());

    dream::pvr::Ta ta;
    ta.reset();
    for (std::size_t i = 0; i + 8 <= words.size(); i += 8) ta.feed(&words[i]);

    std::uint32_t ta_polygons = 0, ta_sprites = 0, ta_vertices = 0;
    for (const auto& l : ta.lists) {
        ta_polygons += l.polygons;
        ta_sprites += l.sprites;
        ta_vertices += l.vertices;
    }
    CHECK(ta.invalid == 0);
    CHECK(dl.frame().sprites == ta_sprites);
    // The runtime counts parameters: one vertex parameter per sprite. This decoder expands each
    // sprite into the four corners of its quad, so it holds three more vertices per sprite.
    CHECK(dl.frame().vertices.size() == ta_vertices + 3u * ta_sprites);
    // Every sprite is a polygon here, and each polygon header contributes at least one strip
    // (a header may be followed by several).
    CHECK(dl.frame().polygon_count() >= ta_sprites);
    CHECK(dl.frame().strips + dl.frame().sprites == dl.frame().polygon_count());
    MESSAGE("runtime parser: " << ta_polygons << " polygon headers, " << ta_sprites << " sprites, "
                               << ta_vertices << " vertex parameters");
}

// The seam at --scale 2: a texture the game maps once across a quad must be addressed
// clamp-to-edge, not repeat, or the outermost half-texel fetches the opposite edge once the
// geometry is drawn above the guest's resolution. The predicate that decides it is tested here
// rather than the sampler, because the sampler needs a GPU and this runs on every CI runner.
TEST_CASE("polygon_tiles tells a mapped texture from a repeated one") {
    Frame frame;
    auto quad = [&frame](float u0, float v0, float u1, float v1) {
        Polygon p;
        p.first = static_cast<u32>(frame.vertices.size());
        p.count = 4;
        for (int i = 0; i < 4; ++i) {
            Vertex v;
            v.u = (i & 1) ? u1 : u0;
            v.v = (i & 2) ? v1 : v0;
            frame.vertices.push_back(v);
        }
        return p;
    };

    // The ordinary case, and the one that was being outlined: exactly one copy across the quad.
    CHECK(polygon_tiles(frame, quad(0.0f, 0.0f, 1.0f, 1.0f)) == false);
    // A sub-rectangle of a larger shared texture, which is how the title logo's 49 quads are cut.
    CHECK(polygon_tiles(frame, quad(0.25f, 0.5f, 0.375f, 0.625f)) == false);
    // The guest's own transform lands a hair either side of the mark; that is not tiling.
    CHECK(polygon_tiles(frame, quad(-0.002f, 0.0f, 1.001f, 1.0f)) == false);
    // Genuine repetition overshoots by whole multiples, in either axis and either direction.
    CHECK(polygon_tiles(frame, quad(0.0f, 0.0f, 4.0f, 1.0f)) == true);
    CHECK(polygon_tiles(frame, quad(0.0f, -3.0f, 1.0f, 1.0f)) == true);

    // A strip whose count runs past the vertices it was given must not read off the end.
    Polygon truncated = quad(0.0f, 0.0f, 1.0f, 1.0f);
    truncated.count = 400;
    CHECK(polygon_tiles(frame, truncated) == false);
}
