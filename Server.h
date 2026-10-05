/*
	Server.h - Slimmer
	Copyright (C) 2016-2017  Terényi, Balázs (terenyi@freemail.hu)

	This program is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef SERVER_H
#define SERVER_H

#include "jsonrpccpp/client.h"
#include "jsonrpccpp/client/connectors/httpclient.h"

using namespace std;

// Talks to a Music Assistant server (HTTP API at /api) and hands back the
// data in the same shape as the old LMS JSON-RPC answers, so the rest of
// slimmer did not have to change.
class Server
{
public:
	enum PlaylistControlCommand {ADD, LOAD};
	enum PlaylistControlType {GENRE, ARTIST, ALBUM, TRACK, YEAR, PLAYLIST, FOLDER, PLAYLISTNAME};

	Server(const string& url, const string& token = "");
	Json::Value artists(const bool albumArtists = false);
	Json::Value albums(const string& artistId = "");
	Json::Value newAlbums();
	Json::Value tracks(const string& albumId);
	Json::Value favorites();
	Json::Value radioPlugins();
	Json::Value folders(const string& folderId = "");

	const string& version() const { return mVersion; }

	Json::Value playerStatus(const string& playerId, const bool fullPlaylist);
	void setPlayerVolume(const string& playerId, const string& volume);
	void playPlaylistItem(const string& playerId, const string& index);
	void removePlaylistItem(const string& playerId, const int index);
	void clearPlaylist(const string& playerId);
	void playFavorite(const string& playerId, const string& favId);
	void pause(const string& playerId);
	void playlistControl(const string& playerId, const PlaylistControlCommand cmd, const PlaylistControlType type, const string& id);
	Json::Value radios(const string& playerId, const string& plugin, const string& id);
	void playRadio(const string& playerId, const PlaylistControlCommand cmd, const string& plugin, const string& id);

protected:
	static const unsigned int cMaxResponseItems = 10000;

	Json::Value call(const string& command, const Json::Value& args = Json::Value(Json::objectValue));
	string queueId(const string& playerId) const;
	void playMedia(const string& playerId, const Json::Value& media, const PlaylistControlCommand cmd);
	Json::Value libraryItems(const string& mediaType, const bool favoritesOnly = false, const string& orderBy = "sort_name", const int limit = cMaxResponseItems);

	jsonrpc::HttpClient mHttpClient;
	string mVersion;
	string mQueueId; // Active queue of the player, learned from the last status query
	int mVolume; // Last known volume, needed because MA only knows absolute volume
	unsigned int mMessageId;
};

#endif // SERVER_H
