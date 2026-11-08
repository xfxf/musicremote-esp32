#include "playlist_cache_policy.hpp"

#include <cassert>
#include <vector>

using musicremote::Playlist;
using musicremote::playlist_cache_policy::Identity;
using musicremote::playlist_cache_policy::resolve;

int main() {
  const std::vector<Playlist> playlists{
      {.id = 0, .count = 62, .name = "Local files"},
      {.id = 2, .count = 48321, .name = "NAS:Music"},
  };

  assert(resolve(Identity{.playlist_id = 2, .count = 48321, .name = "NAS:Music"},
                 playlists) == 2);
  assert(!resolve(Identity{.playlist_id = 2, .count = 48320, .name = "NAS:Music"},
                  playlists));
  assert(!resolve(Identity{.playlist_id = 3, .count = 48321, .name = "NAS:Music"},
                  playlists));

  assert(resolve(Identity{.playlist_id = -1, .count = 48321, .name = "NAS:Music"},
                 playlists) == 2);
  auto duplicate = playlists;
  duplicate.push_back({.id = 3, .count = 48321, .name = "NAS:Music"});
  assert(!resolve(Identity{.playlist_id = -1, .count = 48321, .name = "NAS:Music"},
                  duplicate));

  auto folder = playlists;
  folder[1].folder = true;
  assert(!resolve(Identity{.playlist_id = 2, .count = 48321, .name = "NAS:Music"},
                  folder));
  return 0;
}
