#pragma once

#include <vector>
#include <string>
#include <tchar.h>
#include "ScrobSubmitter.h"


/// The glue between Spotify and Last.fm's API
class Scrobblify {
public:
  Scrobblify() { }

  virtual ~Scrobblify();

  // Initializes Scrobblify and the Last.fm client
  void Init();

  // Get directory containing Spotify data.
  static std::wstring GetSpotifyDirectory();

  // Get directory containing Spotify user directories
  static std::wstring GetSpotifyUsersDirectory();

  // Get the directories for all the Spotify users on this computer
  static size_t GetSpotifyUserDirectories(
      std::vector<std::wstring> &users,
      const std::wstring user_filter = _T("*"));

  // Sends a track for scrobbling
  int Start(const std::wstring& artist, const std::wstring& track);

  // Stop (or pauses) the current track
  int Stop();

private:
  // Method used for receiving information from scrob_submitter_
  static void StatusCallback(int request_id, bool is_error, std::string message, void * user_data);

  // Path to user meta data files
  std::vector<std::wstring> metadata_paths_;

  // ID of current request
  int current_request_id_;

  // Last.fm scrobbler
  ScrobSubmitter scrob_submitter_;

  // Directory from which Scrobblify says it's scrobbling from
  std::string scrobble_directory_; // must be UTF8
};
