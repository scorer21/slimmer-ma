/*
	Server.cpp - Slimmer
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

#include "Server.h"
#include "Config.h"
#include <algorithm>
#include <chrono>
#include <iostream>

using jsonrpc::JsonRpcException;
using jsonrpc::Errors;

namespace
{

// "library://album/12" -> provider "library", item id "12".
// The item id itself may contain slashes (filesystem provider), so only the
// first slash after the media type counts.
void splitUri(const string& uri, string& provider, string& itemId)
{
	size_t sep = uri.find("://");
	if (sep == string::npos)
	{
		provider = "library";
		itemId = uri;
		return;
	}
	provider = uri.substr(0, sep);
	size_t slash = uri.find('/', sep + 3);
	itemId = slash == string::npos ? "" : uri.substr(slash + 1);
}

string uriOf(const Json::Value& item, const string& mediaType)
{
	if (item["uri"].isString() && !item["uri"].asString().empty())
		return item["uri"].asString();
	return item["provider"].asString() + "://" + mediaType + "/" + item["item_id"].asString();
}

string artistNames(const Json::Value& item)
{
	string names;
	const Json::Value& artists = item["artists"];
	if (artists.isArray())
		for (Json::ArrayIndex i = 0; i < artists.size(); i++)
		{
			if (!names.empty()) names += ", ";
			names += artists[i]["name"].asString();
		}
	return names;
}

// One queue item in the shape of an LMS playlist_loop entry
Json::Value queueItemToLms(const Json::Value& queueItem)
{
	Json::Value result;
	const Json::Value& media = queueItem["media_item"];
	result["remote"] = media["media_type"].asString() == "radio" ? "1" : "0";
	result["remote_title"] = "";
	result["title"] = media.isObject() && !media["name"].asString().empty() ? media["name"].asString() : queueItem["name"].asString();
	result["artist"] = artistNames(media);
	result["album"] = media["album"].isObject() ? media["album"]["name"].asString() : "";
	return result;
}

double now()
{
	using namespace std::chrono;
	return duration_cast<duration<double>>(system_clock::now().time_since_epoch()).count();
}

}

Server::Server(const string& url, const string& token) : mHttpClient(url), mVolume(0), mMessageId(0)
{
	if (!token.empty())
		mHttpClient.AddHeader("Authorization", "Bearer " + token);
	mVersion = call("info")["server_version"].asString();
}

Json::Value Server::call(const string& command, const Json::Value& args)
{
	Json::Value request;
	request["command"] = command;
	request["args"] = args;
	request["message_id"] = to_string(++mMessageId);

	Json::FastWriter writer;
	string response;
	mHttpClient.SendRPCMessage(writer.write(request), response);

	Json::Value result;
	Json::Reader reader;
	if (!response.empty() && !reader.parse(response, result))
		throw JsonRpcException(Errors::ERROR_CLIENT_INVALID_RESPONSE, "Invalid answer from Music Assistant to " + command);
	return result;
}

string Server::queueId(const string& playerId) const
{
	return mQueueId.empty() ? playerId : mQueueId;
}

Json::Value Server::libraryItems(const string& mediaType, const bool favoritesOnly, const string& orderBy, const int limit)
{
	Json::Value args;
	args["limit"] = limit;
	args["order_by"] = orderBy;
	if (favoritesOnly)
		args["favorite"] = true;
	return call("music/" + mediaType + "/library_items", args);
}

void Server::playMedia(const string& playerId, const Json::Value& media, const PlaylistControlCommand cmd)
{
	Json::Value args;
	args["queue_id"] = queueId(playerId);
	args["media"] = media;
	args["option"] = cmd == LOAD ? "replace" : "add";
	call("player_queues/play_media", args);
}

Json::Value Server::artists(const bool albumArtists)
{
	Json::Value args;
	args["limit"] = cMaxResponseItems;
	args["album_artists_only"] = albumArtists;
	Json::Value items = call("music/artists/library_items", args);

	Json::Value result(Json::arrayValue);
	for (Json::ArrayIndex i = 0; i < items.size(); i++)
	{
		Json::Value artist;
		artist["id"] = uriOf(items[i], "artist");
		artist["artist"] = items[i]["name"];
		result.append(artist);
	}
	return result;
}

Json::Value Server::albums(const string& artistId)
{
	Json::Value items;
	if (artistId.empty())
		items = libraryItems("albums");
	else
	{
		string provider, itemId;
		splitUri(artistId, provider, itemId);
		Json::Value args;
		args["item_id"] = itemId;
		args["provider_instance_id_or_domain"] = provider;
		items = call("music/artists/artist_albums", args);
	}

	Json::Value result(Json::arrayValue);
	for (Json::ArrayIndex i = 0; i < items.size(); i++)
	{
		Json::Value album;
		album["id"] = uriOf(items[i], "album");
		album["album"] = items[i]["name"];
		result.append(album);
	}
	return result;
}

Json::Value Server::newAlbums()
{
	Json::Value items = libraryItems("albums", false, "timestamp_added_desc", Config::cNewMusicItems);

	Json::Value result(Json::arrayValue);
	for (Json::ArrayIndex i = 0; i < items.size(); i++)
	{
		Json::Value album;
		album["id"] = uriOf(items[i], "album");
		album["album"] = items[i]["name"];
		result.append(album);
	}
	return result;
}

Json::Value Server::tracks(const string& albumId)
{
	string provider, itemId;
	splitUri(albumId, provider, itemId);
	Json::Value args;
	args["item_id"] = itemId;
	args["provider_instance_id_or_domain"] = provider;
	Json::Value items = call("music/albums/album_tracks", args);

	Json::Value result(Json::arrayValue);
	for (Json::ArrayIndex i = 0; i < items.size(); i++)
	{
		Json::Value track;
		track["id"] = uriOf(items[i], "track");
		track["title"] = items[i]["name"];
		result.append(track);
	}
	return result;
}

Json::Value Server::favorites()
{
	// MA has no separate favorites list, favorites are flagged library items
	static const char* const types[] = {"playlists", "radios", "albums", "artists", "tracks"};
	static const char* const singular[] = {"playlist", "radio", "album", "artist", "track"};

	Json::Value result(Json::arrayValue);
	for (int t = 0; t < 5; t++)
	{
		Json::Value items = libraryItems(types[t], true);
		for (Json::ArrayIndex i = 0; i < items.size(); i++)
		{
			Json::Value favorite;
			favorite["id"] = uriOf(items[i], singular[t]);
			favorite["name"] = items[i]["name"];
			result.append(favorite);
		}
	}
	return result;
}

Json::Value Server::radioPlugins()
{
	// The old LMS radio plugins are replaced by the radio stations and
	// playlists of the MA library
	Json::Value result(Json::arrayValue);
	Json::Value plugin;
	plugin["type"] = "xmlbrowser";
	plugin["cmd"] = "radios";
	plugin["name"] = "Radio stations";
	result.append(plugin);
	plugin["cmd"] = "playlists";
	plugin["name"] = "Playlists";
	result.append(plugin);
	return result;
}

Json::Value Server::folders(const string& folderId)
{
	Json::Value args(Json::objectValue);
	if (!folderId.empty())
		args["path"] = folderId;
	Json::Value items = call("music/browse", args);

	Json::Value result(Json::arrayValue);
	for (Json::ArrayIndex i = 0; i < items.size(); i++)
	{
		const Json::Value& item = items[i];
		string name = item["name"].asString();
		if (name.empty() || name == "..") continue;
		Json::Value entry;
		if (item["media_type"].asString() == "folder")
		{
			entry["id"] = item["path"].asString().empty() ? item["uri"].asString() : item["path"].asString();
			entry["type"] = "folder";
		}
		else
		{
			entry["id"] = uriOf(item, item["media_type"].asString());
			entry["type"] = "track";
		}
		entry["filename"] = name;
		result.append(entry);
	}
	return result;
}

Json::Value Server::playerStatus(const string& playerId, const bool fullPlaylist)
{
	Json::Value args;
	args["player_id"] = playerId;
	const Json::Value player = call("players/get", args);
	if (!player.isObject())
		throw JsonRpcException(Errors::ERROR_CLIENT_CONNECTOR, "Player " + playerId + " is unknown at the server");
	const Json::Value queue = call("player_queues/get_active_queue", args);
	mQueueId = queue.isObject() ? queue["queue_id"].asString() : "";

	Json::Value status;
	string state = queue.isObject() ? queue["state"].asString() : player["playback_state"].asString();
	status["mode"] = state == "playing" ? "play" : state == "paused" ? "pause" : "stop";
	status["power"] = player["powered"].isNull() || player["powered"].asBool() ? 1 : 0;

	mVolume = player["volume_level"].isNull() ? 0 : player["volume_level"].asInt();
	status["mixer volume"] = mVolume;

	status["playlist_tracks"] = queue.isObject() ? queue["items"].asInt() : 0;
	status["playlist_cur_index"] = queue.isObject() && !queue["current_index"].isNull() ? queue["current_index"].asString() : "";

	string repeat = queue["repeat_mode"].asString();
	status["playlist repeat"] = repeat == "one" ? 1 : repeat == "all" ? 2 : 0;
	status["playlist shuffle"] = queue["shuffle_enabled"].asBool() ? 1 : 0;

	const Json::Value& current = queue["current_item"];
	status["duration"] = current["duration"].isNull() ? 0 : current["duration"].asInt();
	double elapsed = queue["elapsed_time"].asDouble();
	if (state == "playing" && queue["elapsed_time_last_updated"].isNumeric())
		elapsed += now() - queue["elapsed_time_last_updated"].asDouble();
	if (elapsed < 0) elapsed = 0;
	if (status["duration"].asInt() > 0 && elapsed > status["duration"].asInt()) elapsed = status["duration"].asInt();
	status["time"] = elapsed;

	// What is playing right now. For radio streams the title and artist
	// come from the stream metadata, which MA puts into current_media.
	Json::Value nowPlaying = queueItemToLms(current);
	if (nowPlaying["remote"].asString() == "1")
	{
		const Json::Value& media = player["current_media"];
		nowPlaying["remote_title"] = current["media_item"]["name"].asString();
		nowPlaying["title"] = media["title"].asString();
		nowPlaying["artist"] = media["artist"].asString();
		nowPlaying["album"] = "";
	}

	status["playlist_loop"] = Json::Value(Json::arrayValue);
	if (fullPlaylist && queue.isObject())
	{
		Json::Value itemArgs;
		itemArgs["queue_id"] = mQueueId;
		itemArgs["limit"] = cMaxResponseItems;
		itemArgs["offset"] = 0;
		Json::Value items = call("player_queues/items", itemArgs);
		int currentIndex = queue["current_index"].isNull() ? -1 : queue["current_index"].asInt();
		for (Json::ArrayIndex i = 0; i < items.size(); i++)
			status["playlist_loop"].append((int)i == currentIndex ? nowPlaying : queueItemToLms(items[i]));
	}
	else
		status["playlist_loop"].append(nowPlaying);

	return status;
}

void Server::setPlayerVolume(const string& playerId, const string& volume)
{
	// LMS took "+2" and "-2", MA only takes absolute values
	int level = stoi(volume);
	if (!volume.empty() && (volume[0] == '+' || volume[0] == '-'))
		level += mVolume;
	level = max(0, min(100, level));

	Json::Value args;
	args["player_id"] = playerId;
	args["volume_level"] = level;
	call("players/cmd/volume_set", args);
	mVolume = level;
}

void Server::playPlaylistItem(const string& playerId, const string& index)
{
	Json::Value args;
	args["queue_id"] = queueId(playerId);
	if (index == "+1")
		call("player_queues/next", args);
	else if (index == "-1")
		call("player_queues/previous", args);
	else
	{
		args["index"] = stoi(index);
		call("player_queues/play_index", args);
	}
}

void Server::removePlaylistItem(const string& playerId, const int index)
{
	Json::Value args;
	args["queue_id"] = queueId(playerId);
	args["item_id_or_index"] = index;
	call("player_queues/delete_item", args);
}

void Server::clearPlaylist(const string& playerId)
{
	Json::Value args;
	args["queue_id"] = queueId(playerId);
	call("player_queues/clear", args);
}

void Server::playFavorite(const string& playerId, const string& favId)
{
	playMedia(playerId, favId, LOAD);
}

void Server::pause(const string& playerId)
{
	Json::Value args;
	args["queue_id"] = queueId(playerId);
	call("player_queues/play_pause", args);
}

void Server::playlistControl(const string& playerId, const PlaylistControlCommand cmd, const PlaylistControlType type, const string& id)
{
	if (type != FOLDER)
	{
		// Artists, albums and tracks carry their MA uri as id
		playMedia(playerId, id, cmd);
		return;
	}

	// A folder is not playable as such, so everything playable in it is queued
	Json::Value args;
	args["path"] = id;
	Json::Value items = call("music/browse", args);
	Json::Value media(Json::arrayValue);
	for (Json::ArrayIndex i = 0; i < items.size(); i++)
		if (items[i]["media_type"].asString() != "folder")
			media.append(uriOf(items[i], items[i]["media_type"].asString()));
	if (media.size())
		playMedia(playerId, media, cmd);
}

Json::Value Server::radios(const string& playerId, const string& plugin, const string& id)
{
	Json::Value result(Json::arrayValue);
	if (plugin != "radios" && plugin != "playlists")
		return result;

	Json::Value items = libraryItems(plugin);
	for (Json::ArrayIndex i = 0; i < items.size(); i++)
	{
		Json::Value entry;
		entry["id"] = uriOf(items[i], plugin == "radios" ? "radio" : "playlist");
		entry["name"] = items[i]["name"];
		entry["hasitems"] = 0;
		result.append(entry);
	}
	return result;
}

void Server::playRadio(const string& playerId, const PlaylistControlCommand cmd, const string& plugin, const string& id)
{
	playMedia(playerId, id, cmd);
}
