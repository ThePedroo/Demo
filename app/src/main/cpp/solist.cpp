#include "solist.hpp"
#include "logging.h"

namespace SoList {

ProtectedDataGuard::FuncType ProtectedDataGuard::ctor = NULL;
ProtectedDataGuard::FuncType ProtectedDataGuard::dtor = NULL;

SoInfo *DetectInjection() {
  if (solist == NULL && !Initialize()) {
    LOGE("Failed to initialize solist");
    return NULL;
  }
  SoInfo *prev = solist;
  size_t gap = 0;
  auto gap_repeated = 0;

  SoInfo *stats_jni = NULL;

  for (auto iter = solist; iter; iter = iter->get_next()) {
    // No soinfo has empty path name
    if (iter->get_path()[0] == '\0') {
      return iter;
    }

    if (stats_jni == NULL && strcmp(iter->get_name(), "libstats_jni.so") == 0) {
      stats_jni = iter;
    }

    if (iter - prev != gap && gap_repeated == 0) {
      gap = iter - prev;
    } else if (iter - prev == gap) {
      LOGD("Skip solinfo %p: %s", iter, iter->get_name());
      gap_repeated++;
    } else if (iter - prev == 2 * gap) {
      // A gap appears, indicating that one library was unloaded
      auto dropped = (SoInfo *)((uintptr_t)prev + gap);
      const char *mark =
          "libnativehelper.so"; // The first library to load after AppSpecialize
      if (strcmp(prev->get_name(), mark) == 0 ||
          strcmp(iter->get_name(), mark) == 0)
        return dropped;
    } else {
      gap_repeated--;
      LOGD("Strange gap 0x%lx or 0x%lx != 0x%lx between %s and %s", iter - prev,
           prev - iter, gap, prev->get_name(), iter->get_name());
    }
    prev = iter;
  }

  if (stats_jni == NULL || stats_jni->get_next() != *sonext) {
    // libstats_jni should be loaded exactly before current library libdemo.so
    return stats_jni;
  } else {
    return nullptr;
  }
}

bool Initialize() {
  SandHook::ElfImg linker("/linker");
  if (!ProtectedDataGuard::setup(linker))
    return false;

  std::string_view solist_sym_name =
      linker.findSymbolNameByPrefix("__dl__ZL6solist");
  if (solist_sym_name.empty())
    return false;

  /* INFO: The size isn't a magic number, it's the size for the string:
   * .llvm.7690929523238822858 */
  char llvm_sufix[25 + 1];

  if (solist_sym_name.length() != strlen("__dl__ZL6solist")) {
    strncpy(llvm_sufix, solist_sym_name.data() + strlen("__dl__ZL6solist"),
            sizeof(llvm_sufix));
  } else {
    llvm_sufix[0] = '\0';
  }

  solist = getStaticPointer<SoInfo>(linker, solist_sym_name.data());
  if (solist == NULL)
    return false;

  char somain_sym_name[sizeof("__dl__ZL6somain") + sizeof(llvm_sufix)];
  snprintf(somain_sym_name, sizeof(somain_sym_name), "__dl__ZL6somain%s",
           llvm_sufix);

  char sonext_sym_name[sizeof("__dl__ZL6sonext") + sizeof(llvm_sufix)];
  snprintf(sonext_sym_name, sizeof(somain_sym_name), "__dl__ZL6sonext%s",
           llvm_sufix);

  char vsdo_sym_name[sizeof("__dl__ZL4vdso") + sizeof(llvm_sufix)];
  snprintf(vsdo_sym_name, sizeof(vsdo_sym_name), "__dl__ZL4vdso%s", llvm_sufix);

  somain = getStaticPointer<SoInfo>(linker, somain_sym_name);
  if (somain == NULL)
    return false;

  sonext = linker.getSymbAddress<SoInfo **>(sonext_sym_name);
  if (sonext == NULL)
    return false;

  SoInfo *vsdo = getStaticPointer<SoInfo>(linker, vsdo_sym_name);
  if (vsdo == NULL)
    return false;

  SoInfo::get_realpath_sym =
      reinterpret_cast<decltype(SoInfo::get_realpath_sym)>(
          linker.getSymbAddress("__dl__ZNK6soinfo12get_realpathEv"));
  SoInfo::get_soname_sym = reinterpret_cast<decltype(SoInfo::get_soname_sym)>(
      linker.getSymbAddress("__dl__ZNK6soinfo10get_sonameEv"));

  for (size_t i = 0; i < 1024 / sizeof(void *); i++) {
    auto *possible_next = *(void **)((uintptr_t)solist + i * sizeof(void *));
    if (possible_next == somain || (vsdo != NULL && possible_next == vsdo)) {
      SoInfo::solist_next_offset = i * sizeof(void *);

      break;
    }
  }

  return (SoInfo::get_realpath_sym != NULL && SoInfo::get_soname_sym != NULL);
}
} // namespace SoList