#include "ColorGrade.hpp"
#include "ParameterSampler.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

namespace heisenberg::grade {
namespace {
constexpr ColorGradeParameterSpec specs[] = {
    {"exposure",-3,3,0}, {"contrast",0.5f,1.5f,1},
    {"highlights",-1,1,0}, {"shadows",-1,1,0}, {"blacks",-1,1,0}, {"whites",-1,1,0},
    {"temperature",2000,11000,6500}, {"tint",-100,100,0},
    {"vibrance",-1,1,0}, {"saturation",0,2,1},
    {"liftHue",0,360,0}, {"liftStrength",0,1,0}, {"liftMaster",-0.5f,0.5f,0},
    {"gammaHue",0,360,0}, {"gammaStrength",0,1,0}, {"gammaMaster",0.5f,2,1},
    {"gainHue",0,360,0}, {"gainStrength",0,1,0}, {"gainMaster",0.5f,1.5f,1}
};
constexpr ColorGradeParameterSpec targetSpecs[] = {
    {"hue",0,360,0}, {"rotation",-30,30,0}, {"saturation",0,2,1}, {"lightness",-0.5f,0.5f,0}
};
std::array<float*,19> fields(ColorGradeState& s) {
    return {&s.ev,&s.contrast,&s.highlights,&s.shadows,&s.blacks,&s.whites,
        &s.temperature,&s.tint,&s.vibrance,&s.saturation,
        &s.lift.hue,&s.lift.strength,&s.lift.master,
        &s.gamma.hue,&s.gamma.strength,&s.gamma.master,
        &s.gain.hue,&s.gain.strength,&s.gain.master};
}
bool failure(std::vector<GradeDiagnostic>& diagnostics, const std::string& id,
             const std::string& parameter, const std::string& reason, std::string* error) {
    const auto message = "Color grade '"+id+"' parameter '"+parameter+"': "+reason;
    diagnostics.push_back({"invalid_color_grade",id,parameter,message});
    if (error) *error = message;
    return false;
}
bool validNumber(const JsonValue& value, float low, float high) {
    if (!value.is_number()) return false;
    const double n = value.get<double>();
    return std::isfinite(n) && n >= low && n <= high
        && std::isfinite(static_cast<float>(n)) && (n == 0 || static_cast<float>(n) != 0);
}
bool sampleScalar(const JsonValue* value, const ColorGradeParameterSpec& spec,
                  const std::string& path, int64_t frame, float& output,
                  std::vector<GradeDiagnostic>& diagnostics, const std::string& id,
                  std::string* error, GradeCompileOptions options) {
    if (value) {
        if (!value->is_object()) return failure(diagnostics,id,path,"expected {value,track} parameter object",error);
        if (value->contains("value") && !validNumber((*value)["value"],spec.minimum,spec.maximum))
            return failure(diagnostics,id,path,"base value outside declared range or unrepresentable",error);
        if (value->contains("track") && (*value)["track"].is_object()
            && (*value)["track"].contains("keyframes") && (*value)["track"]["keyframes"].is_array()) {
            for (const auto& key : (*value)["track"]["keyframes"])
                if (key.is_object() && key.contains("value") && !validNumber(key["value"],spec.minimum,spec.maximum))
                    return failure(diagnostics,id,path,"keyframe value outside declared range or unrepresentable",error);
        }
    }
    filtergraph::VulkanParameterValue sampled;
    if (!sampleParameter(value,spec.defaultValue,frame,sampled,diagnostics,id,path,error,options)) return false;
    output = std::get<float>(sampled);
    if (output < spec.minimum || output > spec.maximum)
        return failure(diagnostics,id,path,"sample outside declared range",error);
    return true;
}
bool validateCurve(const ColorGradeCurve& curve, std::string* error) {
    auto fail = [&](const char* reason) { if (error) *error=reason; return false; };
    if (curve.empty()) return true;
    if (curve.size()<2 || curve.size()>kColorGradeMaxCurvePoints) return fail("curve requires 2..64 points or omission");
    float previous = -1;
    for (const auto& p : curve) {
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || p.x<0 || p.x>1 || p.y<0 || p.y>1)
            return fail("curve coordinates must be finite in [0,1]");
        if (p.x<=previous) return fail("curve x coordinates must remain strictly increasing in GPU float precision");
        previous=p.x;
    }
    return true;
}
bool readCurve(const JsonValue& json, ColorGradeCurve& curve, std::string* error) {
    if (!json.is_array()) { if(error) *error="curve must be an array of [x,y] points"; return false; }
    for (const auto& point : json) {
        if (!point.is_array() || point.size()!=2 || !validNumber(point[0],0,1) || !validNumber(point[1],0,1)) {
            if(error) *error="curve point must contain two representable coordinates in [0,1]";
            return false;
        }
        curve.push_back({point[0].get<float>(),point[1].get<float>()});
    }
    if (!validateCurve(curve,error)) return false;
    if (isIdentityColorGradeCurve(curve)) curve.clear();
    return true;
}
} // namespace

std::span<const ColorGradeParameterSpec> colorGradeParameterRegistry() { return specs; }

bool isIdentityColorGradeCurve(const ColorGradeCurve& curve) {
    if (curve.empty()) return true;
    return curve.size()>=2 && curve.front().x==0 && curve.back().x==1
        && std::all_of(curve.begin(),curve.end(),[](const auto& p){ return p.x==p.y; });
}

bool validateColorGradeState(const ColorGradeState& state, std::string* error) {
    if(error) error->clear();
    auto copy=state;
    const auto values=fields(copy);
    for(size_t i=0;i<values.size();++i) {
        const float n=*values[i];
        if(!std::isfinite(n) || n<specs[i].minimum || n>specs[i].maximum) {
            if(error) *error=std::string(specs[i].name)+" outside declared range";
            return false;
        }
    }
    for(const auto* curve : {&state.curves.master,&state.curves.red,&state.curves.green,&state.curves.blue})
        if(!validateCurve(*curve,error)) return false;
    if(state.hueTargets.size()>64) { if(error) *error="at most 64 hue targets are supported"; return false; }
    for(const auto& t : state.hueTargets) {
        const float values[]={t.hue,t.rotation,t.saturation,t.lightness};
        for(size_t i=0;i<4;++i) if(!std::isfinite(values[i]) || values[i]<targetSpecs[i].minimum || values[i]>targetSpecs[i].maximum) {
            if(error) *error="hue target outside declared range";
            return false;
        }
    }
    if(state.lut && (!std::isfinite(state.lut->intensity) || state.lut->intensity<0 || state.lut->intensity>1)) {
        if(error) *error="LUT intensity outside [0,1]";
        return false;
    }
    return true;
}

bool sampleColorGrade(const JsonValue& effect, int64_t frame, ColorGradeState& output,
                      std::vector<GradeDiagnostic>& diagnostics, std::string* error, GradeCompileOptions options) {
    if(error) error->clear();
    const std::string id=effect.is_object() && effect.contains("id") && effect["id"].is_string()
        ? effect["id"].get<std::string>() : "video.colorGrade";
    auto invalid=[&](const std::string& p,const std::string& reason){return failure(diagnostics,id,p,reason,error);};
    if(!effect.is_object()) return invalid("effect","expected an object");
    if(!effect.contains("type") || effect["type"]!=kColorGradeType) return invalid("type","expected video.colorGrade");
    if(!effect.contains("schemaVersion") || !effect["schemaVersion"].is_number_integer() || effect["schemaVersion"]!=1)
        return invalid("schemaVersion","expected top-level integer schemaVersion 1");
    ColorGradeState candidate;
    if(effect.contains("enabled")) {
        if(!effect["enabled"].is_boolean()) return invalid("enabled","expected boolean");
        candidate.enabled=effect["enabled"].get<bool>();
    }
    if(effect.contains("params") && !effect["params"].is_object()) return invalid("params","expected an object");
    const JsonValue empty=JsonValue::object();
    const auto& params=effect.contains("params")?effect["params"]:empty;
    const auto destinations=fields(candidate);
    for(size_t i=0;i<destinations.size();++i) {
        const auto& spec=specs[i];
        const JsonValue* value=params.contains(spec.name)?&params[spec.name]:nullptr;
        if(!sampleScalar(value,spec,spec.name,frame,*destinations[i],diagnostics,id,error,options)) return false;
    }
    for(float* hue : {&candidate.lift.hue,&candidate.gamma.hue,&candidate.gain.hue}) if(*hue==360) *hue=0;
    if(params.contains("curves")) {
        const auto& curves=params["curves"];
        if(!curves.is_object()) return invalid("curves","expected master/red/green/blue object");
        const char* names[]={"master","red","green","blue"};
        ColorGradeCurve* dest[]={&candidate.curves.master,&candidate.curves.red,&candidate.curves.green,&candidate.curves.blue};
        for(size_t i=0;i<4;++i) if(curves.contains(names[i])) {
            std::string reason;
            if(!readCurve(curves[names[i]],*dest[i],&reason)) return invalid(std::string("curves.")+names[i],reason);
        }
    }
    if(params.contains("hueTargets")) {
        const auto& targets=params["hueTargets"];
        if(!targets.is_array() || targets.size()>64) return invalid("hueTargets","expected an array of at most 64 targets");
        for(size_t index=0;index<targets.size();++index) {
            const auto& t=targets[index];
            if(!t.is_object()) return invalid("hueTargets","expected target objects");
            ColorGradeHueTarget target;
            float* dest[]={&target.hue,&target.rotation,&target.saturation,&target.lightness};
            for(size_t i=0;i<4;++i) {
                const auto& spec=targetSpecs[i];
                const std::string path="hueTargets["+std::to_string(index)+"]."+spec.name;
                const auto* value=t.contains(spec.name)?&t[spec.name]:nullptr;
                JsonValue wrapped;
                if(value && value->is_number()) { wrapped={{"value",*value}}; value=&wrapped; }
                if(!sampleScalar(value,spec,path,frame,*dest[i],diagnostics,id,error,options)) return false;
            }
            if(target.hue==360) target.hue=0;
            candidate.hueTargets.push_back(target);
        }
    }
    for(auto it=params.begin();it!=params.end();++it) {
        const auto& key=it.key();
        const bool known=key=="curves" || key=="hueTargets" || key=="lut"
            || std::any_of(std::begin(specs),std::end(specs),[&](const auto& s){return key==s.name;});
        if(!known) {
            const std::string message="Unsupported color grade parameter '"+key+"'";
            diagnostics.push_back({"unsupported_parameter",id,key,message});
            if(options.strict) { if(error) *error=message; return false; }
        }
    }
    output=std::move(candidate);
    return true;
}

bool canonicalizeColorGrade(const JsonValue& effect, JsonValue& output, std::string* error) {
    ColorGradeState state;
    std::vector<GradeDiagnostic> diagnostics;
    if(!sampleColorGrade(effect,0,state,diagnostics,error,{.strict=false})) return false;
    auto candidate=effect;
    if(!candidate.contains("enabled")) candidate["enabled"]=true;
    if(!candidate.contains("params")) candidate["params"]=JsonValue::object();
    auto& params=candidate["params"];
    for(const auto& spec : specs) {
        if(!params.contains(spec.name)) params[spec.name]={{"value",spec.defaultValue}};
        else if(!params[spec.name].contains("value")) params[spec.name]["value"]=spec.defaultValue;
        if(std::string(spec.name).find("Hue")!=std::string::npos && params[spec.name]["value"]==360)
            params[spec.name]["value"]=0;
    }
    if(params.contains("curves")) {
        for(const char* name : {"master","red","green","blue"}) if(params["curves"].contains(name)) {
            ColorGradeCurve curve;
            if(!readCurve(params["curves"][name],curve,error)) return false;
            if(curve.empty()) params["curves"].erase(name);
        }
        if(params["curves"].empty()) params.erase("curves");
    }
    if(params.contains("hueTargets")) for(auto& target : params["hueTargets"]) {
        for(const auto& spec : targetSpecs) if(!target.contains(spec.name)) target[spec.name]=spec.defaultValue;
        if(target["hue"]==360) target["hue"]=0;
        else if(target["hue"].is_object() && target["hue"].contains("value") && target["hue"]["value"]==360)
            target["hue"]["value"]=0;
    }
    output=std::move(candidate);
    return true;
}

} // namespace heisenberg::grade
