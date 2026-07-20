//
// Created by NiceFold on 2026/7/20.
//

#pragma once

// ============================================================
// DLL 导出宏
// ============================================================
#ifdef _WIN32
    #if defined(FILTERGRAPH_EXPORT_DEFINE)
        #define FG_EXPORT __declspec(dllexport)
    #else
        #define FG_EXPORT __declspec(dllimport)
    #endif
#elif __ANDROID__
    #if defined(FILTERGRAPH_EXPORT_DEFINE)
        #define FG_EXPORT __attribute__((visibility("default")))
    #else
        #define FG_EXPORT
    #endif
#endif

#ifdef _WIN32
    #define FG_DLL_EXPORT __declspec(dllexport)
#elif __ANDROID__
    #define FG_DLL_EXPORT __attribute__((visibility("default")))
#else
    #define FG_DLL_EXPORT
#endif

// ============================================================
// 层查询接口宏 — 解决 VkInputLayer/VkOutputLayer 的钻石继承问题
//
// ILayer (虚基类)
//   ├── InputLayer / OutputLayer
//   └── BaseLayer → VkLayer
//        └── VkInputLayer / VkOutputLayer
//
// 用法：在叶子类 public 区域写 AOCE_LAYER_QUERYINTERFACE(ClassName)
// ============================================================

#define FG_LAYER_QUERYINTERFACE(OBJCLASS)                \
    virtual inline IBaseLayer* getLayer() override {     \
        OBJCLASS* obj = static_cast<OBJCLASS*>(this);    \
        return static_cast<BaseLayer*>(obj);             \
    }

#define FG_LAYER_GETNAME(OBJCLASS)                       \
    virtual inline const char* getName() override {      \
        return #OBJCLASS;                                \
    }

// ============================================================
// 模块注册宏
// ============================================================

#if FILTERGRAPH_USE_STATIC
    #define ADD_FILTER_MODULE(ModuleClass, ModuleName)                         \
        static heisenberg::filtergraph::StaticLinkModule<ModuleClass>          \
            LinkModule##ModuleName(#ModuleName);
#else
    #define ADD_FILTER_MODULE(ModuleClass, ModuleName)   \
        extern "C" FG_DLL_EXPORT IModule* NewModule() {  \
            return new ModuleClass();                    \
        }
#endif

// ============================================================
// Vulkan 参数更新宏
// ============================================================

#define FG_VULKAN_PARAMETUPDATE()                                          \
    virtual inline void onUpdateParamet() override {                       \
        if (bParametMatch) {                                                \
            if (paramet == oldParamet) {                                   \
                return;                                                    \
            }                                                              \
            if (constBufCpu.size() == sizeof(paramet)) {                  \
                memcpy(constBufCpu.data(), &paramet, sizeof(paramet));    \
            }                                                              \
            bParametChange = true;                                          \
        }                                                                  \
    }
