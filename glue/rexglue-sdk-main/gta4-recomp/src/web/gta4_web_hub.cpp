// [web port] The PS5 hub in the browser (ps5/hub): shown before the game is
// set up, as on the PS5 (gta4_ps5_app.cpp ShowHub), with the PS5's own pages.
// The hub's pictures, music and fonts are the PS5 package's (ps5/package/hub,
// ps5/assets/fonts), served to the page and mounted at /app0.
#include <rex/platform.h>

#if REX_PLATFORM_WEB

#include <filesystem>
#include <functional>
#include <string>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/rex_app.h>

#include "../gta4_app.h"
#include "../ps5/game/save_import.h"
#include "../ps5/hub/hub_lang.h"
#include "../ps5/hub/ps5_hub.h"

// The PS5 build's settings the hub shows and the start reads
// (ps5/gta4_ps5_app.cpp defines them there).
REXCVAR_DEFINE_STRING(ps5_language, "auto", "PS5",
                      "Game and hub language: auto, english, french, german, spanish, italian");
REXCVAR_DEFINE_DOUBLE(ps5_population, 1.0, "PS5", "Cars and people in the city: 1.0 is the Xbox 360's")
    .range(0.25, 2.0);
REXCVAR_DECLARE(uint32_t, user_language);

namespace gta4web {

namespace fs = std::filesystem;
namespace hub = gta4ps5::hub;

void ShowHub(rex::ui::ImGuiDrawer* drawer, rex::ui::ImmediateDrawer* immediate,
             const fs::path& liberty_root, const fs::path& config, std::function<void()> launch) {
  std::error_code ec;
  fs::create_directories(config.parent_path(), ec);
  // (the Saves and Trophies pages look in these: present, so lookups don't go
  // through missing directories in the browser's storage)
  fs::create_directories(liberty_root / "saves" / "achievements", ec);
  hub::Paths hub_paths;
  hub_paths.app = "/app0";
  hub_paths.config = config;
  hub_paths.user_data = liberty_root / "saves";
  hub_paths.install = liberty_root;
  // Xbox 360 saves, from the import folder on the computer the page is served
  // from (as the PS5 app's, gta4_ps5_app.cpp); no refresh-rate switch here
  const fs::path saves_root = hub_paths.user_data;
  hub::SetSaveImport({[] { return gta4ps5::PendingSaveImports("/app0"); },
                      [saves_root] {
                        const gta4ps5::SaveImportResult r = gta4ps5::ImportXbox360Saves("/app0", saves_root);
                        std::string message;
                        if (r.imported)
                          message = hub::lang::F(r.imported == 1 ? "{} save imported." : "{} saves imported.",
                                                 {std::to_string(r.imported)}) +
                                    " " + hub::lang::T("Load them from the game's menu.");
                        else
                          message = hub::lang::T("No saves were imported.");
                        for (size_t i = 0; i < r.errors.size() && i < 4; ++i) message += "\n" + r.errors[i];
                        if (r.errors.size() > 4) message += std::string("\n") + hub::lang::T("...the log lists the rest.");
                        return message;
                      }});
  hub::Show(drawer, immediate, hub_paths, [config, launch = std::move(launch)] {
    rex::cvar::LoadConfig(config);
    const hub::lang::Lang language = hub::lang::FromSetting(REXCVAR_GET(ps5_language));
    REXCVAR_SET(user_language, hub::lang::XboxLanguage(language));
    const std::string population = fmt::format("{}", REXCVAR_GET(ps5_population));
    for (const char* scale : {"gta4_traffic_density_scale", "gta4_parked_car_density_scale",
                              "gta4_ped_density_scale", "gta4_scenario_ped_density_scale"}) {
      rex::cvar::SetFlagByName(scale, population);
    }
    REXLOG_INFO("web hub: START GAME (language {}, population {})", hub::lang::Name(language),
                population);
    launch();
  });
}

}  // namespace gta4web

void GTA4App::OnConfigureFonts(ImFontAtlas* atlas) { gta4ps5::hub::ConfigureFonts(atlas); }

#endif  // REX_PLATFORM_WEB
