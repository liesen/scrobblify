#include "Scrobblify.h"
#include "ScrobSubmitter.h"
#include <tchar.h>
#include <string>
#include <vector>
#include <shlobj.h>
#include <strsafe.h>
#include <sstream>
#include <fstream>


// Scrobblify's plugin id
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

/** 
 * Initializes Scrobblify. Collects paths to meta-data, sets the scrobble 
 * directory and initializes the connection to the Last.fm client.
 */
void Scrobblify::Init() {
  // Collect meta-data files
  Scrobblify::GetSpotifyUserDirectories(metadata_paths_);

  for (std::vector<std::wstring>::iterator it = metadata_paths_.begin();
       it != metadata_paths_.end();
       ++it) {
    *it += _T("/metadata"); // Here we could check if the file exists
  }

  scrobble_directory_ = ToUtf8(Scrobblify::GetSpotifyDirectory());

  // Initialize Last.fm
  scrob_submitter_.Init(kPluginId, &Scrobblify::StatusCallback, (void *) this);
}

/** 
 * Callback function for messages from the Last.fm client. Declared static
 * because of type issues when using pointer to member functions as callbacks;
 * /user_data/ is a pointer to the sending Scrobblify instance (butt ugly).
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

  // Search all the meta-data files available
  for (std::vector<std::wstring>::const_iterator it = metadata_paths_.begin();
       it != metadata_paths_.end();
       ++it) {
    // Go get some meta data
    FILE *metadata_file;

    if (_wfopen_s(&metadata_file, (*it).c_str(), _T("r")) != 0) {
      continue;
    }

    std::wifstream in(metadata_file);
    const size_t kMaxLineLength = 2048;
    wchar_t line[kMaxLineLength];
    
    // Skip first line (contains "21" for no apparent reason)
    in.getline(line, kMaxLineLength);

    // Skip if file is empty
    if (wcsnlen(line, 1) == 0) {
      in.close();
      continue;
    }

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
      continue; // Try next file
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
         in.getline(line, kMaxLineLength)) { }

    // Parse songs section. Each song is formatted like
    //   hash        32 bytes
    //   track       
    //   artists     ; split with 0x2
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
        wchar_t *p = &line[kHashLength + kDelimiterLength + track.size() +
            kDelimiterLength];
        
        // Match any of the artists
        bool artist_match_found = wcsncmp(artist_hash, p, kHashLength) == 0;

        for (p += kHashLength;
             *p == 0x2;
             p += kHashLength + kDelimiterLength) {
          if (!artist_match_found && // Don't compare if artist has been found
              wcsncmp(artist_hash, &p[1], kHashLength) == 0) {
            artist_match_found = true;
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
      continue;
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
    return current_request_id_ = scrob_submitter_.Start(
        ToUtf8(artist),
        ToUtf8(track),
        ToUtf8(album),
        "",
        length,
        scrobble_directory_);
  }

  // No meta-data found; fall back on a five minute track length
  // TODO(liesen): does this violate any AudioScrobbler rules?
  return current_request_id_ = scrob_submitter_.Start(
        ToUtf8(artist),
        ToUtf8(track),
        "",
        "",
        5 * 60, // five minutes -- more than most songs
        scrobble_directory_);
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
    throw std::runtime_error("Could not find the Application Data directory.");
  }

  // Find %APPDATA%/Spotify
  StringCchCat(path, MAX_PATH, _T("/Spotify"));
  WIN32_FIND_DATA find_data;
  HANDLE find_handle = FindFirstFile(path, &find_data);

  if (INVALID_HANDLE_VALUE == find_handle) {
    throw std::runtime_error("Could not locate Spotify settings directory.");
  }

  FindClose(find_handle);
  return std::wstring(path);
}

/**
 * Returns the directory containing Spotify users' "home" directories. Sub-
 * directories of this directory contain the meta-data file for each user.
 */
std::wstring Scrobblify::GetSpotifyUsersDirectory() {
  return GetSpotifyDirectory() + _T("/Users");
}

/**
 * Pushes all Spotify user directories onto /users/.
 */
size_t Scrobblify::GetSpotifyUserDirectories(std::vector<std::wstring> &users,
                                             const std::wstring user_filter) {
  std::wstring users_directory = GetSpotifyUsersDirectory();
  
  // Find users; directories named <username>-user
  std::wstring path = users_directory + _T("/") + user_filter + _T("-user");
  size_t num_users = 0;
  WIN32_FIND_DATA find_data;
  HANDLE find_handle = FindFirstFile(path.c_str(), &find_data);
  
  do {
    if (find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
      users.push_back(users_directory + find_data.cFileName);
      ++num_users;
    }
  } while (FindNextFile(find_handle, &find_data));

  FindClose(find_handle);
  return num_users;
}
