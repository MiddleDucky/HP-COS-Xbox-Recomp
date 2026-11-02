#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "gpu/state.h"

// HLSL for the title's shaders and for its fixed function state, per the XDK's definitions of both
namespace gpu {

enum TexKind : uint8_t { kTexNone, kTex2D, kTex3D, kTexCube };

struct TexInfo {
    uint8_t kind = kTexNone;
    uint8_t rect = 0;    // linear texture, coordinates in texels
    uint8_t shadow = 0;  // depth texture: 1 D24, 2 D16
    uint8_t flags = 0;   // kTexHdr, kTexNormCube, kTexWide, kTexBorderU/V, kTexOpaque
};
enum : uint8_t {
    kTexHdr = 1,       // a render target holding HDR colour divided by the headroom
    kTexNormCube = 2,  // a normalization cube: computed per pixel instead, with HDR
    kTexWide = 4,      // an upscaled target shrunk by the pass: each tap averaged over console texels
    // xboxrecomp: border addressing done in the shader, past 0..1 the stage's border colour; the sampler clamps.
    // vkd3d's sampler border colour came out black for HP:CoS's shadow target (whose view forced alpha to 1),
    // so all ground beyond the shadow map's reach was drawn black
    kTexBorderU = 8,
    kTexBorderV = 16,
    kTexOpaque = 32,   // xboxrecomp: a format without alpha read from a target: alpha 1, set in the shader
};

struct PixelKey {
    uint32_t programmable = 0;
    // combiners, straight from D3DPIXELSHADERDEF
    uint32_t count = 0, rgb_in[8] = {}, rgb_out[8] = {}, a_in[8] = {}, a_out[8] = {}, final_abcd = 0, final_efg = 0;
    uint32_t tex_modes = 0, dot_mapping = 0, input_texture = 0, compare_mode = 0;
    // fixed function stages: colorop, arg1, arg2, arg0, alphaop, arg1, arg2, arg0, result
    uint32_t ff[4][9] = {};
    TexInfo tex[4];
    uint32_t alphakill = 0;  // bit per stage
    uint32_t flat = 0, two_sided = 0;
    uint32_t proj = 0;       // fixed function: bit per stage sampled at coordinates divided by w
    uint32_t quad = 0;       // a screen quad: QuadHlsl
    uint32_t wbuf = 0;       // xboxrecomp: W buffered, the shader writes w / far W as depth
    bool operator==(const PixelKey& o) const;
};

uint64_t Hash(const void* p, size_t n, uint64_t h = 1469598103934665603ull);

PixelKey MakePixelKey(const State& s, const TexInfo tex[4]);

// xboxrecomp: whether PixelHlsl's shader for the draw samples stage i's texture at all
bool StageSampled(const State& s, int i);

// xboxrecomp: D3DRS_ZENABLE is D3DZB_USEW: the draw's pixel shader writes depth from w (PixelHlsl)
bool WBuffered(const State& s);

// vertex program microcode to HLSL, regs is the bit mask of v registers the draw supplies
// decode: VertexDecode per register, 4 bits each, for elements the input assembler hands over raw
std::string VertexProgramHlsl(const std::vector<uint32_t>& program, uint16_t regs, uint64_t decode = 0, bool ppl = false);
// xboxrecomp: per pixel lighting (shaders.cpp PerPixelFields): whether a program can be run again per pixel for its
// colours, and what PixelHlsl needs for it
bool PerPixelLightable(const std::vector<uint32_t>& program, uint16_t regs);
struct PerPixelSource {
    const std::vector<uint32_t>* prog;
    uint16_t regs;
};
// fixed function vertex state a shader is built for
struct FixedKey {
    uint32_t light = 0;    // bit 0 lighting, 1 specular, 2 local viewer, 3 normalize, then D3DMCS 2 bits each from bit 8:
                           // diffuse, specular, ambient, emissive (material when the vertex lacks that colour)
    uint32_t types = 0;    // D3DLIGHTTYPE per light, 2 bits each, 0 off
    uint32_t tex[4] = {};  // per stage: coordinate set, texgen at bit 4, D3DTTFF count at bit 8, projected bit 11, set size at bit 12
};
FixedKey MakeFixedKey(const State& s);
std::string FixedVertexHlsl(uint16_t regs, bool pretransformed, uint64_t decode = 0, const FixedKey& f = {});
std::string PixelHlsl(const PixelKey& k, const PerPixelSource* ppl = nullptr);
std::string QuadHlsl(std::string s, uint32_t texels = 0);  // texels: stages with coordinates in texels
std::string LineGsHlsl(std::string vs);  // a line draw's geometry shader, entry "gsline", over its vertex shader

// the constant buffer layout both stages share, register b1
struct DrawConstants {
    float surf[4];      // target width, height, 1 / depth scale, 0
    float fog[4];       // start, end, density, mode
    float fog_color[4];
    float misc[4];      // alpha ref, alpha func, shadow func, specular
    float pc[18][4];    // combiner c0 c1 per stage, then final c0 c1
    float tfactor[4];
    float texsize[4][4];
    float bump[4][4];   // 00 01 10 11
    float bumpl[4][4];  // scale offset
    float wvp[16];      // fixed function
    float hdr[4];       // colour out scale, HDR target read scale, 0, 0
    float spread[4][4]; // kTexWide: tap step in uv, taps per axis
    // fixed function, lit in view space
    float wv[16];
    float wvn[16];       // normals: inverse transpose of wv
    float tm[4][16];     // texture transforms
    float mat[5][4];     // material diffuse, ambient, specular, emissive, power
    float amb[4];        // D3DRS_AMBIENT
    float lt[8][7][4];   // per light: diffuse, specular, ambient, position range, direction falloff, attenuation, cos theta/2 phi/2
    float wbuf[4];       // xboxrecomp: W buffered draws: 1, offset, slope scale (depth units, 0..1), 0
    float border[4][4];  // xboxrecomp: D3DTSS_BORDERCOLOR per stage, for kTexBorderU/V
    float uvrect[4][4];  // xboxrecomp: a single quad's texture rectangle per stage (u0 v0 u1 v1), 0 = none; QuadHlsl
    float qrect[4];      // xboxrecomp: a single screen space quad's guest pixel rectangle (x0 y0 x1 y1), 0 = none
    float squeeze[4];    // xboxrecomp: 16:9, 2D drawn over the scene: x scale about x centre (scale, centre), 0 = none
};

}  // namespace gpu
