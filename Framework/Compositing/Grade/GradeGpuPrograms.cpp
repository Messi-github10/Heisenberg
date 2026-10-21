#include "GradeGpuPrograms.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace heisenberg::grade {
namespace {
using Vec = std::array<float,3>;
using Matrix = std::array<std::array<double,3>,3>;
constexpr Matrix unit{{{1,0,0},{0,1,0},{0,0,1}}};
Matrix multiply(const Matrix& a,const Matrix& b) {
    Matrix r{};
    for(size_t i=0;i<3;++i) for(size_t j=0;j<3;++j) for(size_t k=0;k<3;++k) r[i][j]+=a[i][k]*b[k][j];
    return r;
}
std::array<double,3> transform(const Matrix& a,const std::array<double,3>& v) {
    std::array<double,3> r{};
    for(size_t i=0;i<3;++i) for(size_t j=0;j<3;++j) r[i]+=a[i][j]*v[j];
    return r;
}
Matrix inverse(const Matrix& a) {
    const double d=a[0][0]*(a[1][1]*a[2][2]-a[1][2]*a[2][1])-a[0][1]*(a[1][0]*a[2][2]-a[1][2]*a[2][0])+a[0][2]*(a[1][0]*a[2][1]-a[1][1]*a[2][0]);
    Matrix r{};
    r[0][0]=(a[1][1]*a[2][2]-a[1][2]*a[2][1])/d;
    r[0][1]=(a[0][2]*a[2][1]-a[0][1]*a[2][2])/d;
    r[0][2]=(a[0][1]*a[1][2]-a[0][2]*a[1][1])/d;
    r[1][0]=(a[1][2]*a[2][0]-a[1][0]*a[2][2])/d;
    r[1][1]=(a[0][0]*a[2][2]-a[0][2]*a[2][0])/d;
    r[1][2]=(a[0][2]*a[1][0]-a[0][0]*a[1][2])/d;
    r[2][0]=(a[1][0]*a[2][1]-a[1][1]*a[2][0])/d;
    r[2][1]=(a[0][1]*a[2][0]-a[0][0]*a[2][1])/d;
    r[2][2]=(a[0][0]*a[1][1]-a[0][1]*a[1][0])/d;
    return r;
}
std::array<double,3> white(double temperature,double tint=0) {
    const double t=temperature;
    const double x=t<=4000 ? -0.2661239e9/(t*t*t)-0.2343580e6/(t*t)+0.8776956e3/t+0.179910
                          : -3.0258469e9/(t*t*t)+2.1070379e6/(t*t)+0.2226347e3/t+0.240390;
    double y=t<=2222 ? -1.1063814*x*x*x-1.34811020*x*x+2.18555832*x-0.20219683
             : t<=4000 ? -0.9549476*x*x*x-1.37418593*x*x+2.09137015*x-0.16748867
                       : 3.0817580*x*x*x-5.87338670*x*x+3.75112997*x-0.37001483;
    y-=tint*0.0001;
    return {x/y,1,(1-x-y)/y};
}
Matrix adaptation(const std::array<double,3>& source,const std::array<double,3>& destination) {
    constexpr Matrix bradford{{{0.8951,0.2664,-0.1614},{-0.7502,1.7135,0.0367},{0.0389,-0.0685,1.0296}}};
    const auto s=transform(bradford,source),d=transform(bradford,destination);
    Matrix diagonal{};for(size_t i=0;i<3;++i) diagonal[i][i]=d[i]/s[i];
    return multiply(inverse(bradford),multiply(diagonal,bradford));
}
Matrix whiteBalance(const ColorGradeState& s) {
    if(s.temperature==6500 && s.tint==0) return unit;
    constexpr Matrix ap1ToXyz{{{0.6624541811,0.1340042065,0.1561876870},{0.2722287168,0.6740817658,0.0536895174},{-0.0055746495,0.0040607335,1.0103391003}}};
    const std::array<double,3> d60{0.32168/0.33767,1,(1-0.32168-0.33767)/0.33767};
    const auto reference=white(6500);
    const auto d60ToReference=adaptation(d60,reference);
    const auto relative=adaptation(white(s.temperature,s.tint),reference);
    auto matrix=multiply(inverse(ap1ToXyz),multiply(inverse(d60ToReference),multiply(relative,multiply(d60ToReference,ap1ToXyz))));
    const auto neutral=transform(matrix,{1,1,1});
    const double luminance=neutral[0]*ap1ToXyz[1][0]+neutral[1]*ap1ToXyz[1][1]+neutral[2]*ap1ToXyz[1][2];
    for(auto& row:matrix) for(double& v:row) v/=luminance;
    return matrix;
}
constexpr Vec weights{0.2126f,0.7152f,0.0722f};
constexpr float cctBlack=0.0729055341958355f, cctWhite=0.5547945205479452f;
float dot(const Vec& v) { return v[0]*weights[0]+v[1]*weights[1]+v[2]*weights[2]; }
float smooth(float a,float b,float x) { const float t=std::clamp((x-a)/(b-a),0.0f,1.0f); return t*t*(3-2*t); }
float wrap(float h) { return h-360*std::floor(h/360); }
Vec hueRgb(float h) {
    h=wrap(h)/60;
    const float x=1-std::abs(std::fmod(h,2.0f)-1);
    if(h<1) return {1,x,0}; if(h<2) return {x,1,0}; if(h<3) return {0,1,x};
    if(h<4) return {0,x,1}; if(h<5) return {x,0,1}; return {1,0,x};
}
Vec opponent(const ColorWheelState& wheel) {
    auto v=hueRgb(wheel.hue); const float y=dot(v);
    for(float& c:v) c-=y;
    return v;
}
float curveValue(const ColorGradeCurve& curve,float x) {
    if(isIdentityColorGradeCurve(curve)) return x;
    if(x<=curve.front().x) return curve.front().y;
    if(x>=curve.back().x) return curve.back().y;
    const auto right=std::upper_bound(curve.begin(),curve.end(),x,[](float n,const auto& p){return n<p.x;});
    const auto& a=*(right-1); const auto& b=*right;
    return a.y+(b.y-a.y)*((x-a.x)/(b.x-a.x));
}
std::vector<ColorGradeHueTarget> sortedTargets(const ColorGradeState& state) {
    auto result=state.hueTargets;
    for(auto& t:result) t.hue=wrap(t.hue);
    // Stable bytes and accumulation order even if the UI reorders target rows.
    std::sort(result.begin(),result.end(),[](const auto& a,const auto& b){
        return std::array{a.hue,a.rotation,a.saturation,a.lightness}<std::array{b.hue,b.rotation,b.saturation,b.lightness};
    });
    return result;
}
void appendBytes(renderer::color::GpuProgram& p,const std::vector<float>& data) {
    p.uniformData.resize(data.size()*sizeof(float));
    std::memcpy(p.uniformData.data(),data.data(),p.uniformData.size());
}
std::string identity(const renderer::color::GpuProgram& p) {
    // Explicit little-endian FNV-1a over immutable shader/resource descriptions;
    // deliberately excludes UBO contents. No implementation-defined std::hash.
    uint64_t hash=14695981039346656037ull;
    auto byte=[&](uint8_t b){hash^=b;hash*=1099511628211ull;};
    auto integer=[&](uint32_t n){for(int i=0;i<4;++i) byte(uint8_t(n>>(i*8)));};
    for(unsigned char c:p.shaderText) byte(c);
    for(unsigned char c:p.functionName) byte(c);
    integer(uint32_t(p.uniformData.size()));
    for(const auto& l:p.luts) {
        integer(l.binding);integer(l.width);integer(l.height);integer(l.depth);integer(l.channels);
        integer(l.is3D);integer(l.linearInterpolation);
        for(float f:l.values) integer(std::bit_cast<uint32_t>(f));
    }
    std::ostringstream text; text<<"color-grade-v1-"<<std::hex<<std::setw(16)<<std::setfill('0')<<hash;
    return text.str();
}
const char* linearShader=R"GLSL(
layout(std140,set=1,binding=0) uniform GradeLinear { vec4 controls; vec4 wb0; vec4 wb1; vec4 wb2; } gradeLinear;
vec4 colorGradeLinear(vec4 pixel) {
    mat3 wb=mat3(gradeLinear.wb0.rgb,gradeLinear.wb1.rgb,gradeLinear.wb2.rgb);
    return vec4((wb*pixel.rgb)*exp2(gradeLinear.controls.x),pixel.a);
}
)GLSL";
const char* toneShader=R"GLSL(
layout(std140,set=1,binding=0) uniform GradeTone { vec4 a; vec4 b; } gradeTone;
vec4 colorGradeTone(vec4 pixel) {
    vec3 rgb=(pixel.rgb-vec3(0.4135884024924423))*gradeTone.a.x+vec3(0.4135884024924423);
    // AP1 luminance; masks are queried once, never from partly adjusted pixels.
    float y=dot(rgb,vec3(0.2722287168,0.6740817658,0.0536895174));
    float u=clamp((y-0.0729055341958355)/(0.5547945205479452-0.0729055341958355),0.0,1.0);
    float delta=0.12*gradeTone.a.y*smoothstep(0.35,0.9,u)
        +0.12*gradeTone.a.z*(1.0-smoothstep(0.1,0.6,u))
        +0.08*gradeTone.a.w*(1.0-smoothstep(0.0,0.25,u))
        +0.08*gradeTone.b.x*smoothstep(0.75,1.0,u);
    return vec4(rgb+vec3(delta),pixel.a);
}
)GLSL";
const char* auxiliaryShader=R"GLSL(
layout(std140,set=1,binding=0) uniform GradeAuxiliary {
    vec4 presence;
    vec4 lift;
    vec4 gamma;
    vec4 gain;
    vec4 targets[64];
} gradeAux;
layout(set=1,binding=1) uniform sampler2D gradeCurves;
const vec3 gradeY=vec3(0.2126,0.7152,0.0722);
vec3 gradeHueRgb(float h) {
    return clamp(abs(mod(h/60.0+vec3(0.0,4.0,2.0),6.0)-3.0)-1.0,0.0,1.0);
}
float gradeCurve(float x,int row) {
    int n=int(texelFetch(gradeCurves,ivec2(0,row),0).b);
    if(n==0) return x;
    vec2 first=texelFetch(gradeCurves,ivec2(0,row),0).rg;
    vec2 last=texelFetch(gradeCurves,ivec2(n-1,row),0).rg;
    if(x<=first.x) return first.y;
    if(x>=last.x) return last.y;
    int lo=0,hi=n-1;
    for(int i=0;i<6;++i) {
        if(hi-lo<=1) break;
        int mid=(lo+hi)/2;
        if(x<texelFetch(gradeCurves,ivec2(mid,row),0).r) hi=mid; else lo=mid;
    }
    vec2 a=texelFetch(gradeCurves,ivec2(lo,row),0).rg;
    vec2 b=texelFetch(gradeCurves,ivec2(hi,row),0).rg;
    return a.y+(b.y-a.y)*((x-a.x)/(b.x-a.x));
}
vec3 gradeMasterCurve(vec3 rgb) {
    if(int(texelFetch(gradeCurves,ivec2(0,0),0).b)==0) return rgb;
    float y=dot(rgb,gradeY);
    float mapped=gradeCurve(y,0);
    // Exact chromaticity scaling outside the toe. Near Y=0, fade chroma
    // smoothly to neutral rather than amplifying cancelling signed channels.
    float gate=smoothstep(0.0,0.0001,abs(y));
    float scale=y==0.0?0.0:(gate/y)*mapped;
    return vec3(mapped)+(rgb-vec3(y))*scale;
}
vec4 colorGradeAuxiliary(vec4 pixel) {
    vec3 rgb=pixel.rgb;
    float y=dot(rgb,gradeY);
    float chroma=max(rgb.r,max(rgb.g,rgb.b))-min(rgb.r,min(rgb.g,rgb.b));
    float sat=clamp(chroma/max(max(abs(y),chroma),1e-6),0.0,1.0);
    float vibrance=gradeAux.presence.x;
    float scale=gradeAux.presence.y*(1.0+vibrance*(vibrance>=0.0?1.0-sat:1.0));
    if(scale!=1.0) rgb=vec3(y)+(rgb-vec3(y))*scale;
    // Signed power, inverse gamma. No max(rgb,0), including near black.
    rgb=rgb+gradeAux.lift.rgb*(vec3(1.0)-rgb);
    rgb*=gradeAux.gain.rgb;
    rgb=sign(rgb)*pow(abs(rgb),vec3(1.0)/gradeAux.gamma.rgb);
    rgb=gradeMasterCurve(rgb);
    rgb=vec3(gradeCurve(rgb.r,1),gradeCurve(rgb.g,2),gradeCurve(rgb.b,3));
    float hi=max(rgb.r,max(rgb.g,rgb.b)),lo=min(rgb.r,min(rgb.g,rgb.b));
    float c=hi-lo;
    if(c>0.0 && gradeAux.presence.z>0.0) {
        float h;
        if(hi==rgb.r) h=60.0*mod((rgb.g-rgb.b)/c,6.0);
        else if(hi==rgb.g) h=60.0*((rgb.b-rgb.r)/c+2.0);
        else h=60.0*((rgb.r-rgb.g)/c+4.0);
        h=mod(h+360.0,360.0);
        float rotation=0.0,saturation=0.0,lightness=0.0;
        for(int i=0;i<int(gradeAux.presence.z);++i) {
            vec4 t=gradeAux.targets[i];
            float d=abs(mod(h-t.x+180.0,360.0)-180.0);
            float w=max(1.0-d/22.0,0.0);
            rotation+=w*t.y; saturation+=w*(t.z-1.0); lightness+=w*t.w;
        }
        float gate=smoothstep(0.00001,0.001,c);
        rotation=clamp(rotation,-30.0,30.0)*gate;
        float s=1.0+clamp(saturation,-1.0,1.0)*gate;
        float l=clamp(lightness,-0.5,0.5)*gate;
        float oldY=dot(rgb,gradeY);
        vec3 turned=gradeHueRgb(mod(h+rotation+360.0,360.0))*c+vec3(lo);
        turned+=vec3(oldY-dot(turned,gradeY));
        rgb=vec3(oldY)+(turned-vec3(oldY))*s+vec3(l);
    }
    return vec4(rgb,pixel.a);
}
)GLSL";
std::array<Vec,3> wheelVectors(const ColorGradeState& s) {
    auto lift=opponent(s.lift),gamma=opponent(s.gamma),gain=opponent(s.gain);
    for(size_t c=0;c<3;++c) {
        lift[c]=s.lift.master+0.25f*s.lift.strength*lift[c];
        gamma[c]=s.gamma.master*std::exp2(0.5f*s.gamma.strength*gamma[c]);
        gain[c]=s.gain.master*std::exp2(0.5f*s.gain.strength*gain[c]);
    }
    return {lift,gamma,gain};
}
} // namespace

renderer::color::GpuProgram makeColorGradeGpuProgram(const ColorGradeState& state,ColorGradeProgramDomain domain) {
    std::string error;
    if(!validateColorGradeState(state,&error)) throw std::invalid_argument(error);
    renderer::color::GpuProgram p;
    if(domain==ColorGradeProgramDomain::LinearACEScg) {
        p.shaderText=linearShader;p.functionName="colorGradeLinear";
        const auto wb=whiteBalance(state);
        std::vector<float> uniforms(16,0); uniforms[0]=state.ev;
        for(size_t column=0;column<3;++column) for(size_t row=0;row<3;++row) uniforms[4+column*4+row]=float(wb[row][column]);
        appendBytes(p,uniforms);
    } else if(domain==ColorGradeProgramDomain::ToneACEScct) {
        p.shaderText=toneShader;p.functionName="colorGradeTone";
        appendBytes(p,{state.contrast,state.highlights,state.shadows,state.blacks,state.whites,0,0,0});
    } else if(domain==ColorGradeProgramDomain::Auxiliary) {
        p.shaderText=auxiliaryShader;p.functionName="colorGradeAuxiliary";
        std::vector<float> uniforms(16+64*4,0);
        uniforms[0]=state.vibrance;uniforms[1]=state.saturation;uniforms[2]=float(state.hueTargets.size());
        const auto wheels=wheelVectors(state);
        for(size_t w=0;w<3;++w) for(size_t c=0;c<3;++c) uniforms[4+w*4+c]=wheels[w][c];
        const auto targets=sortedTargets(state);
        for(size_t i=0;i<targets.size();++i) {
            uniforms[16+i*4]=targets[i].hue; uniforms[17+i*4]=targets[i].rotation;
            uniforms[18+i*4]=targets[i].saturation; uniforms[19+i*4]=targets[i].lightness;
        }
        appendBytes(p,uniforms);
        renderer::color::GpuLut lut;
        lut.binding=1;lut.width=64;lut.height=4;lut.channels=3;lut.linearInterpolation=false;
        lut.values.resize(64*4*3,0);
        const ColorGradeCurve* curves[]={&state.curves.master,&state.curves.red,&state.curves.green,&state.curves.blue};
        for(size_t row=0;row<4;++row) {
            const auto& curve=*curves[row];
            if(isIdentityColorGradeCurve(curve)) continue;
            for(size_t i=0;i<curve.size();++i) {
                const size_t offset=(row*64+i)*3;
                lut.values[offset]=curve[i].x;lut.values[offset+1]=curve[i].y;
            }
            lut.values[row*64*3+2]=float(curve.size());
        }
        p.luts.push_back(std::move(lut));
    } else throw std::invalid_argument("Unknown color grade domain");
    p.cacheId=identity(p);
    return p;
}

bool buildColorGradeGpuPrograms(const ColorGradeState& state,std::vector<ColorGradeGpuStage>& output,std::string* error) {
    if(!validateColorGradeState(state,error)) return false;
    std::vector<ColorGradeGpuStage> result;
    if(state.enabled) {
        if(state.ev!=0 || state.temperature!=6500 || state.tint!=0)
            result.push_back({"ACEScg",makeColorGradeGpuProgram(state,ColorGradeProgramDomain::LinearACEScg)});
        if(state.contrast!=1 || state.highlights!=0 || state.shadows!=0 || state.blacks!=0 || state.whites!=0)
            result.push_back({"ACEScct",makeColorGradeGpuProgram(state,ColorGradeProgramDomain::ToneACEScct)});
        const bool wheels=state.lift.master!=0 || state.lift.strength!=0 || state.gamma.master!=1 || state.gamma.strength!=0
            || state.gain.master!=1 || state.gain.strength!=0;
        const bool curves=!isIdentityColorGradeCurve(state.curves.master) || !isIdentityColorGradeCurve(state.curves.red)
            || !isIdentityColorGradeCurve(state.curves.green) || !isIdentityColorGradeCurve(state.curves.blue);
        const bool hue=std::any_of(state.hueTargets.begin(),state.hueTargets.end(),[](const auto& t){return t.rotation!=0 || t.saturation!=1 || t.lightness!=0;});
        if(state.vibrance!=0 || state.saturation!=1 || wheels || curves || hue)
            result.push_back({kColorGradeAuxiliarySpace,makeColorGradeGpuProgram(state,ColorGradeProgramDomain::Auxiliary)});
    }
    output=std::move(result);
    return true;
}

std::array<float,4> referenceColorGradePixel(const ColorGradeState& s,ColorGradeProgramDomain domain,std::array<float,4> pixel) {
    std::string error;
    if(!validateColorGradeState(s,&error)) throw std::invalid_argument(error);
    Vec rgb{pixel[0],pixel[1],pixel[2]};
    if(domain==ColorGradeProgramDomain::LinearACEScg) {
        const auto balanced=transform(whiteBalance(s),{rgb[0],rgb[1],rgb[2]});
        for(size_t c=0;c<3;++c) rgb[c]=float(balanced[c])*std::exp2(s.ev);
    } else if(domain==ColorGradeProgramDomain::ToneACEScct) {
        for(float& c:rgb) c=(c-0.4135884024924423f)*s.contrast+0.4135884024924423f;
        const float y=rgb[0]*0.2722287168f+rgb[1]*0.6740817658f+rgb[2]*0.0536895174f;
        const float u=std::clamp((y-cctBlack)/(cctWhite-cctBlack),0.0f,1.0f);
        const float d=0.12f*s.highlights*smooth(0.35f,0.9f,u)+0.12f*s.shadows*(1-smooth(0.1f,0.6f,u))
            +0.08f*s.blacks*(1-smooth(0,0.25f,u))+0.08f*s.whites*smooth(0.75f,1,u);
        for(float& c:rgb) c+=d;
    } else if(domain==ColorGradeProgramDomain::Auxiliary) {
        const float y=dot(rgb);
        const float chroma=*std::max_element(rgb.begin(),rgb.end())-*std::min_element(rgb.begin(),rgb.end());
        const float sat=std::clamp(chroma/std::max({std::abs(y),chroma,1e-6f}),0.0f,1.0f);
        const float scale=s.saturation*(1+s.vibrance*(s.vibrance>=0?1-sat:1));
        const auto wheels=wheelVectors(s);
        const ColorGradeCurve* channels[]={&s.curves.red,&s.curves.green,&s.curves.blue};
        for(size_t c=0;c<3;++c) {
            if(scale!=1) rgb[c]=y+(rgb[c]-y)*scale;
            rgb[c]=(rgb[c]+wheels[0][c]*(1-rgb[c]))*wheels[2][c];
            rgb[c]=std::copysign(std::pow(std::abs(rgb[c]),1/wheels[1][c]),rgb[c]);
        }
        if(!isIdentityColorGradeCurve(s.curves.master)) {
            const float luminance=dot(rgb);
            const float mapped=curveValue(s.curves.master,luminance);
            const float gate=smooth(0,0.0001f,std::abs(luminance));
            const float scale=luminance==0 ? 0 : (gate/luminance)*mapped;
            for(float& channel:rgb) channel=mapped+(channel-luminance)*scale;
        }
        for(size_t c=0;c<3;++c) rgb[c]=curveValue(*channels[c],rgb[c]);
        const float hi=*std::max_element(rgb.begin(),rgb.end()),lo=*std::min_element(rgb.begin(),rgb.end()),c=hi-lo;
        if(c>0 && !s.hueTargets.empty()) {
            float h;
            if(hi==rgb[0]) h=60*std::fmod((rgb[1]-rgb[2])/c,6.0f);
            else if(hi==rgb[1]) h=60*((rgb[2]-rgb[0])/c+2);
            else h=60*((rgb[0]-rgb[1])/c+4);
            h=wrap(h);
            float rotation=0,saturation=0,lightness=0;
            for(const auto& t:sortedTargets(s)) {
                const float d=std::abs(wrap(h-t.hue+180)-180);
                const float w=std::max(1-d/22,0.0f);
                rotation+=w*t.rotation;saturation+=w*(t.saturation-1);lightness+=w*t.lightness;
            }
            const float gate=smooth(0.00001f,0.001f,c);
            rotation=std::clamp(rotation,-30.0f,30.0f)*gate;
            const float saturationScale=1+std::clamp(saturation,-1.0f,1.0f)*gate;
            const float l=std::clamp(lightness,-0.5f,0.5f)*gate;
            const float oldY=dot(rgb);
            auto turned=hueRgb(h+rotation);
            for(float& channel:turned) channel=channel*c+lo;
            const float delta=oldY-dot(turned);
            for(size_t i=0;i<3;++i) rgb[i]=oldY+(turned[i]+delta-oldY)*saturationScale+l;
        }
    } else throw std::invalid_argument("Unknown color grade domain");
    for(size_t c=0;c<3;++c) pixel[c]=rgb[c];
    return pixel;
}

} // namespace heisenberg::grade
