#pragma once

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

// Resolves the INode diamond for concrete nodes using virtual inheritance.
#define FG_NODE_QUERYINTERFACE(OBJCLASS)                 \
    virtual inline IBaseNode* getNode() override {       \
        OBJCLASS* obj = static_cast<OBJCLASS*>(this);    \
        return static_cast<BaseNode*>(obj);              \
    }

#define FG_NODE_GETNAME(OBJCLASS)                        \
    virtual inline const char* getName() override {      \
        return #OBJCLASS;                                \
    }

#if FILTERGRAPH_USE_STATIC
    #define ADD_FILTER_MODULE(ModuleClass, ModuleName)                         \
        static heisenberg::filtergraph::StaticLinkModule<ModuleClass>          \
            LinkModule##ModuleName(#ModuleName);
#else
    #define ADD_FILTER_MODULE(ModuleClass, ModuleName)   \
        extern "C" FG_DLL_EXPORT IModule* NewModule() { \
            return new ModuleClass();                    \
        }
#endif

#define FG_VULKAN_PARAMETUPDATE()                                          \
    virtual inline void onUpdateParamet() override {                       \
        if (bParametMatch) {                                                \
            if (paramet == oldParamet) {                                   \
                return;                                                    \
            }                                                              \
            if (constBufCpu.size() == sizeof(paramet)) {                   \
                memcpy(constBufCpu.data(), &paramet, sizeof(paramet));     \
            }                                                              \
            bParametChange = true;                                         \
        }                                                                  \
    }
