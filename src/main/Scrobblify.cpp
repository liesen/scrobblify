#include "Scrobblify.h"
#include "ScrobSubmitter.h"
#include <tchar.h>
#include <string>
#include <vector>
#include <shlobj.h>
#include <strsafe.h>
#include <sstream>
#include <fstream>


static const std::string kPluginId = "spt";

// Utility function to convert a wstring to string (UTF8)
std::string ToUtf8(const std::wstring& widestring) {
  // Determine the length necessary for the string in UTF8
  int utf8_length = WideCharToMultiByte(CP_UTF8, 0, widestring.c_str(), -1, 
                                        NULL, 0, NULL, NULL);
  
  if (utf8_length == 0) {
    throw std::runtime_error("Error in determining length of UTF8 string");
  }

  std::vector<char> result(utf8_length);
  int length = WideCharToMultiByte(CP_UTF8, 0, widestring.c_str(), -1, 
                                   &result[0], utf8_length, NULL, NULL);

  if (length != utf8_length) {
    throw std::runtime_error("Error in conversion to UTF8");
  }

  return std::string(&result[0]);
}

/** */
Scrobblify::~Scrobblify() {
  scrob_submitter_.Term();
}

/** */
void Scrobblify::Init(const std::wstring& user_id) {
  // Get the path to the user's metadata file
  user_metadata_path_ = user_id + _T("/metadata");

  // Bail out if metadata file does not exist
  WIN32_FIND_DATA find_data;

  if (INVALID_HANDLE_VALUE == 
      FindFirstFile(user_metadata_path_.c_str(), &find_data)) {
    throw std::invalid_argument("User meta-data file not found.");
  }

  scrob_submitter_.Init(kPluginId, &Scrobblify::StatusCallback, (void *) this); // This is so ugly
}

/** 
 * Callback function for messages from the Last.fm client. Declared static
 * because of type issues when using pointer to member functions as callbacks;
 * /user_data/ is a pointer to the sending Scrobblify instance.
 */
void Scrobblify::StatusCallback(int request_id, 
                                bool is_error, 
                                std::string message, 
                                void *user_data) {
  if (is_error) {
    throw std::runtime_error(message);
  } 
}

/**
 * Scrobbles a track. Also finds meta data, length and (possibly) album, for
 * the current track.
 */
int Scrobblify::Start(const std::wstring& artist,
                      const std::wstring& track) {
  // Handle currently playing track
  if (current_request_id_ > 0) {
    Stop();
  }

  // Lines are divided into fields terminated by 0x1
  const wchar_t kDelimiterChar = 0x1;
  const wchar_t kDelimiter[] = {kDelimiterChar, _T('\0')};
  const size_t kDelimiterLength = 1;
  const size_t kHashLength = 32;
  const size_t kLongHashLength = 40;

  // Go get some meta data
  FILE *metadata_file;

  if (_wfopen_s(&metadata_file,
                user_metadata_path_.c_str(),
                _T("r")) != 0) {
    throw std::runtime_error("Could not open user's meta-data file.");
  }

  std::wifstream in(metadata_file);
  const size_t kMaxLineLength = 2048;
  wchar_t line[kMaxLineLength];
  
  // Skip first line (contains "20" for no apparent reason)
  in.getline(line, kMaxLineLength);

  // Find hash for artist
  wchar_t artist_hash[kHashLength];
  bool artist_hash_found = false;

  for (;
       wcsnlen(line, kMaxLineLength) > 0;
       in.getline(line, kMaxLineLength)) {
    if (wcsncmp(artist.c_str(), 
                &line[kHashLength + kDelimiterLength],
                artist.size()) == 0) {
      artist_hash_found = true;
      wcsncpy(artist_hash, line, kHashLength);
      break;
    }
  }

  if (!artist_hash_found) {
    in.close();
    throw std::invalid_argument("Artist meta-data not found.");
  }

  // Skip rest of artist section
  for (in.getline(line, kMaxLineLength);
       wcsnlen(line, kMaxLineLength) > 0;
       in.getline(line, kMaxLineLength)) {}
  
  in.getline(line, kMaxLineLength); // Skip section separator
  std::wifstream::pos_type album_section_position = in.tellg();
  
  // Skip albums section -- move to next empty line
  for (;
       wcsnlen(line, kMaxLineLength) > 0;
       in.getline(line, kMaxLineLength)) {}

  // Parse songs section. Each song is formatted like
  //   hash        32 bytes
  //   track       
  //   artists  ; split with 0x2
  //   long hash
  //   length      
  //   ...
  int length = 0;
  bool track_found = false;
  wchar_t album_hash[kHashLength];

  for (in.getline(line, kMaxLineLength);
       wcsnlen(line, kMaxLineLength) > 0;
       in.getline(line, kMaxLineLength)) {
    if (wcsncmp(track.c_str(),
                &line[kHashLength + kDelimiterLength],
                track.size()) == 0) {
      // Skip hash, separator, track, separator.
      TCHAR *p = &line[kHashLength + kDelimiterLength + track.size() +
          kDelimiterLength];
      
      // Match any of the artists
      bool artist_match_found = wcsncmp(artist_hash, p, kHashLength) == 0;

      for (p += kHashLength;
           *p == 0x2;
           p += kHashLength + kDelimiterLength) {
        if (wcsncmp(artist_hash, &p[1], kHashLength) == 0) {
          artist_match_found = true;
          // Need to step through all artists
        }
      }

      if (!artist_match_found) {
        continue;
      }

      p += kDelimiterLength + kLongHashLength + kDelimiterLength;

      // Read track length
      length = _wtoi(p); 
      track_found = true;

      // Skip length
      p = wcschr(p, kDelimiterChar);
      p += kDelimiterLength;

      // Skip track number
      p = wcschr(p, kDelimiterChar);
      p += kDelimiterLength;

      // Get album hash
      wcsncpy(album_hash, p, kHashLength);
      break;
    }
  }

  if (!track_found || length <= 0) {
    in.close();
    throw std::invalid_argument("Track meta-data not found.");
  }

  // Get album
  in.seekg(album_section_position); // Seek back to the album section
  std::wstring album;

  for (in.getline(line, kMaxLineLength);
       wcsnlen(line, kMaxLineLength) > 0;
       in.getline(line, kMaxLineLength)) {
    if (wcsncmp(album_hash, line, kHashLength) == 0) {
      size_t album_length = wcscspn(&line[kHashLength + kDelimiterLength],
                                    kDelimiter);
      album.assign(line, kHashLength + kDelimiterLength, album_length);
      break;
    }
  }

  in.close();

  current_request_id_ = scrob_submitter_.Start(
      ToUtf8(artist),
      ToUtf8(track),
      ToUtf8(album),
      "",
      length,
      "");

  return current_request_id_;
}

/** */
int Scrobblify::Stop() {
  current_request_id_ = -1;
  return scrob_submitter_.Stop();
}

/**
 * Finds the Spotify folder inside Application Data (%APPDATA%). Fails with 
 * exceptions if no such directory exists.
 */
std::wstring Scrobblify::GetSpotifyDirectory() {
  TCHAR path[MAX_PATH];

  if (FAILED(SHGetSpecialFolderPath(NULL, path, CSIDL_APPDATA, false))) {
    // TODO(liesen): better error
    throw std::runtime_error("Could not find the Application Data directory.");
  }

  // Find %APPDATA%/Spotify
  StringCchCat(path, MAX_PATH, _T("/Spotify"));
  WIN32_FIND_DATA find_data;
  HANDLE find_handle = FindFirstFile(path, &find_data);

  if (INVALID_HANDLE_VALUE == find_handle) {
    // TODO(liesen): better error, plz
    throw std::runtime_error("Could not locate Spotify.");
  }

  FindClose(find_handle);
  return std::wstring(path);
}

// TODO(liesen): make diff between GetSpotifyUsersDirectory vs 
// GetSpotifyUserDirectories clear

/**
 * Returns the directory containing Spotify users.
 */
std::wstring Scrobblify::GetSpotifyUsersDirectory() {
  return GetSpotifyDirectory() + _T("/Users");
}

/**
 * Pushes all Spotify user directories onto /users/.
 */
size_t Scrobblify::GetSpotifyUserDirectories(std::vector<std::wstring> &users,
                                             const std::wstring user_filter) {
  std::wstring users_directory = GetSpotifyUsersDirectory() + _T("/");
  
  // Find users; directories named <username>-user
  std::wstring path = users_directory + user_filter + _T("-user");
  size_t num_users = 0;
  WIN32_FIND_DATA find_data;
  HANDLE find_handle = FindFirstFile(path.c_str(), &find_data);
  
  if (find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
    users.push_back(users_directory + find_data.cFileName);
    ++num_users;
  }

  while (FindNextFile(find_handle, &find_data)) {
    if (find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
      users.push_back(users_directory + find_data.cFileName);
      ++num_users;
    }
  }

  FindClose(find_handle);
  return num_users;
}
