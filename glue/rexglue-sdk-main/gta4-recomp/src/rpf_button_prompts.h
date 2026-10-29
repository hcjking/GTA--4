#pragma once

#include <filesystem>

namespace rex {
class Runtime;
}

namespace gta4::button_prompts {

// Builds (or reuses) a controller-specific shadow xbox360.rpf and redirects
// only that archive path in the guest VFS. The installed archive is read-only.
bool PrepareAndMount(rex::Runtime& runtime, const std::filesystem::path& game_data_root,
                     const std::filesystem::path& cache_root);

// Whether the keyboard (keycap) set is the one mounted.
bool KeyboardPromptsMounted();
// The Controls page's "XBOX GUIDE" label as the chosen set's button (or
// none), in the loaded text: call with the pause menu open.
void PatchLoadedGuideLabel(uint8_t* base);

}  // namespace gta4::button_prompts
