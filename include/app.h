#pragma once
#include "types.h"
#include "tui.h"
#include "provider.h"
#include "enrich.h"
#include "player.h"
#include "input.h"
#include "config.h"
#include "library.h"
#include "theme.h"

namespace ytui {

class App {
public:
    // provider_override comes from --provider and wins over config.json.
    explicit App(Theme theme, const std::string& provider_override = "");
    ~App();
    int run();
    void force_cleanup();
    void set_player_options(const PlayerOptions& opts);
    // Override the thumbnail graphics mode from the CLI (--gfx). Re-resolves
    // against the terminal. Empty string leaves the config value in place.
    void set_graphics_mode(const std::string& mode);
    void set_ui_mode(const std::string& mode);

private:
    AppState state_;
    TUI      tui_;
    std::unique_ptr<Provider> provider_;
    Player   player_;
    InputHandler input_;
    Config   config_;
    Library  library_;
    Enricher enricher_;

    // True when the slow per-video metadata pass should run for a search.
    // Resolved once from config_.enrich against the active graphics mode.
    bool enrich_active() const;
    // Merge whatever the background enricher has finished into state_.results,
    // and queue thumbnails for any URLs that just arrived. Per-frame, non-blocking.
    void pump_enrichment();

    void build_actions();
    void build_playlist_actions();
    void do_search();
    void execute_action(Action action);
    void execute_playlist_action(Action action);
    void open_in_browser(const std::string& url);
    void copy_to_clipboard(const std::string& text);
    void prefetch_thumbnails();
    void resolve_graphics();
    // Force-disable features the terminal can't actually do (colour, chafa,
    // sixel/kitty/iterm), unless config.force_features overrides. Pass
    // authoritative=true once ncurses has reported the real colour count.
    void apply_capability_overrides(bool authoritative);
    void show_browser_picker();
    void show_playlist_picker();
    void enter_playlist(const std::string& playlist_id);
    void refresh_playlist_names();
    void apply_sort_filter();
    void do_save(int choice);

    // In-app settings UI (Ctrl-S): live theme switching + accelerator rebinding.
    void handle_settings_key(int ch);
    void apply_theme_live(const std::string& theme_name);  // re-theme, no restart
};

} // namespace ytui
