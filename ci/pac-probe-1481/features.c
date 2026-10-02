#include <ptrauth.h>
#define FEATURE(name) probe_##name = __has_feature(name)
FEATURE(ptrauth_calls);
FEATURE(ptrauth_returns);
FEATURE(ptrauth_intrinsics);
#define SHOW(name) probe_##name = name
#ifdef __APPLE__
SHOW(__APPLE__);
#else
probe___APPLE__ = undefined;
#endif
#ifdef __aarch64__
SHOW(__aarch64__);
#else
probe___aarch64__ = undefined;
#endif
#ifdef __arm64__
SHOW(__arm64__);
#else
probe___arm64__ = undefined;
#endif
#ifdef __arm64e__
SHOW(__arm64e__);
#else
probe___arm64e__ = undefined;
#endif
#ifdef __PTRAUTH__
SHOW(__PTRAUTH__);
#else
probe___PTRAUTH__ = undefined;
#endif
#ifdef __ARM_FEATURE_PAUTH
SHOW(__ARM_FEATURE_PAUTH);
#else
probe___ARM_FEATURE_PAUTH = undefined;
#endif
#ifdef __ARM_FEATURE_PAC_DEFAULT
SHOW(__ARM_FEATURE_PAC_DEFAULT);
#else
probe___ARM_FEATURE_PAC_DEFAULT = undefined;
#endif
