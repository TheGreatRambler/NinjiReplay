#define SK_GL
#include "glad/glad.h"

#define SK_RELEASE
#include <SDL.h>
#include <codec/SkCodec.h>
#include <core/SkBitmap.h>
#include <core/SkCanvas.h>
#include <core/SkFont.h>
#include <core/SkGraphics.h>
#include <core/SkImage.h>
#include <core/SkSurface.h>
#include <core/SkTextBlob.h>
#include <core/SkTypeface.h>
#include <curl/curl.h>
#include <encode/SkPngEncoder.h>
#include <filesystem>
#include <gpu/GrBackendSurface.h>
#include <gpu/GrDirectContext.h>
#include <gpu/gl/GrGLInterface.h>
#include <iostream>
#include <sqlite3.h>
#include <unordered_map>
#include <unordered_set>
#include <utils/SkRandom.h>
#include <zlib.h>

#if defined(SK_BUILD_FOR_ANDROID)
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#elif defined(SK_BUILD_FOR_UNIX)
#include <GL/gl.h>
#elif defined(SK_BUILD_FOR_MAC)
#include <OpenGL/gl.h>
#elif defined(SK_BUILD_FOR_IOS)
#include <OpenGLES/ES2/gl.h>
#endif

bool gzip_decompress(uint8_t* input, int input_size, std::vector<uint8_t>& output) {
	output.clear();

	unsigned full_length = input_size;
	unsigned half_length = input_size / 2;

	unsigned uncompLength = full_length;
	char* uncomp          = (char*)calloc(sizeof(char), uncompLength);

	z_stream strm;
	strm.next_in   = input;
	strm.avail_in  = input_size;
	strm.total_out = 0;
	strm.zalloc    = Z_NULL;
	strm.zfree     = Z_NULL;

	bool done = false;

	if(inflateInit2(&strm, (32 + MAX_WBITS)) != Z_OK) {
		free(uncomp);
		return false;
	}

	while(!done) {
		// If our output buffer is too small
		if(strm.total_out >= uncompLength) {
			// Increase size of output buffer
			char* uncomp2 = (char*)calloc(sizeof(char), uncompLength + half_length);
			memcpy(uncomp2, uncomp, uncompLength);
			uncompLength += half_length;
			free(uncomp);
			uncomp = uncomp2;
		}

		strm.next_out  = (Bytef*)(uncomp + strm.total_out);
		strm.avail_out = uncompLength - strm.total_out;

		// Inflate another chunk.
		int err = inflate(&strm, Z_SYNC_FLUSH);
		if(err == Z_STREAM_END)
			done = true;
		else if(err != Z_OK) {
			break;
		}
	}

	if(inflateEnd(&strm) != Z_OK) {
		free(uncomp);
		return false;
	}

	std::copy(uncomp, uncomp + strm.total_out, std::back_inserter(output));

	free(uncomp);
	return true;
}

void toLittleEndian(uint32_t& ui) {
	ui = (ui >> 24) | ((ui << 8) & 0x00FF0000) | ((ui >> 8) & 0x0000FF00) | (ui << 24);
}

void toLittleEndianShort(uint16_t& ui) {
	ui = (ui >> 8) | (ui << 8);
}

static size_t write_cb(char* data, size_t n, size_t l, void* userp) {
	((std::string*)userp)->append(data, n * l);
	return n * l;
}

void downloadMiis(std::vector<std::string>& miis_to_download, std::vector<int>& miis_to_download_player,
	std::unordered_map<int, std::string>& mii_images) {
	CURLM* cm;
	CURLMsg* msg;
	unsigned int transfers = 0;
	int msgs_left          = -1;
	int still_alive        = 1;

	constexpr int MAX_PARALLEL = 10;
	const int NUM_URLS         = miis_to_download.size();

	curl_global_init(CURL_GLOBAL_ALL);
	cm = curl_multi_init();

	/* Limit the amount of simultaneous connections curl should allow: */
	curl_multi_setopt(cm, CURLMOPT_MAXCONNECTS, (long)MAX_PARALLEL);

	for(transfers = 0; transfers < MAX_PARALLEL; transfers++) {
		CURL* eh = curl_easy_init();
		curl_easy_setopt(eh, CURLOPT_WRITEFUNCTION, write_cb);
		curl_easy_setopt(eh, CURLOPT_WRITEDATA, &mii_images[transfers]);
		curl_easy_setopt(eh, CURLOPT_URL, miis_to_download[transfers].c_str());
		// curl_easy_setopt(eh, CURLOPT_PRIVATE, miis_to_download_player[transfers]);
		curl_multi_add_handle(cm, eh);
	}

	do {
		curl_multi_perform(cm, &still_alive);

		while((msg = curl_multi_info_read(cm, &msgs_left))) {
			if(msg->msg == CURLMSG_DONE) {
				CURL* e = msg->easy_handle;
				// long player;
				// curl_easy_getinfo(msg->easy_handle, CURLINFO_PRIVATE, &player);
				//  fprintf(stderr, "R: %d - %s <%s>\n", msg->data.result, curl_easy_strerror(msg->data.result), url);
				// char* url;
				// curl_easy_getinfo(msg->easy_handle, CURLINFO_EFFECTIVE_URL, &url);
				// std::cout << "Handled " << url << std::endl;
				curl_multi_remove_handle(cm, e);
				curl_easy_cleanup(e);
			} else {
				fprintf(stderr, "E: CURLMsg (%d)\n", msg->msg);
			}
			if(transfers < NUM_URLS) {
				CURL* eh = curl_easy_init();
				curl_easy_setopt(eh, CURLOPT_WRITEFUNCTION, write_cb);
				curl_easy_setopt(eh, CURLOPT_WRITEDATA, &mii_images[transfers]);
				curl_easy_setopt(eh, CURLOPT_URL, miis_to_download[transfers].c_str());
				// curl_easy_setopt(eh, CURLOPT_PRIVATE, miis_to_download_player[transfers]);
				curl_multi_add_handle(cm, eh);
				transfers++;
			}
		}
		if(still_alive)
			curl_multi_wait(cm, NULL, 0, 1000, NULL);
	} while(still_alive || (transfers < NUM_URLS));

	curl_multi_cleanup(cm);
	curl_global_cleanup();
}

int main(int argc, char* argv[]) {
	std::cout << "Starting" << std::endl;

	sqlite3* db;
	char* err_msg = 0;
	sqlite3_stmt* res;

	int rc = sqlite3_open("E:/MariOver/dump/dump.db", &db);

	if(rc != SQLITE_OK) {
		fprintf(stderr, "Cannot open database: %s\n", sqlite3_errmsg(db));
		sqlite3_close(db);
		return 1;
	}

	char* replay_query = "SELECT data_id,pid,time,replay FROM ninji";
	rc                 = sqlite3_prepare_v2(db, replay_query, -1, &res, 0);
	if(rc != SQLITE_OK) {
		std::cout << "Sqlite could not prepare query" << std::endl;
		return -1;
	}

	enum NinjiFrameInfo : int8_t {
		NONE           = -1,
		DEATH_UNK1     = 10,
		DOOR           = 11,
		PIPE_SUBWORLD  = 0,
		PIPE_OVERWORLD = 1,
	};
	struct __attribute__((packed, aligned(8))) NinjiFrame {
		uint8_t state;
		uint16_t x;
		uint16_t y;
		NinjiFrameInfo info = NinjiFrameInfo::NONE;
	};

	struct __attribute__((packed, aligned(8))) NinjiInfo {
		std::string name;
		std::string code;
		std::string country;
		SkBitmap* mii_image;
	};

	struct __attribute__((packed, aligned(8))) NinjiGlobalInfo {
		// 0: Mario
		// 1: Luigi
		// 2: Toad
		// 3: Toadette
		uint8_t charactor;
	};

	struct LevelBounds {
		int start_x;
		int start_y;
	};

	std::unordered_set<int> levels_to_render = { 33883306 };
	std::unordered_map<int, std::unordered_map<int, std::vector<NinjiFrame>>> ninji_paths;
	std::unordered_map<int, std::unordered_map<int, bool>> ninji_is_subworld;
	int current_player_index = 0;
	std::unordered_map<std::string, int> pid_to_player;
	std::unordered_map<int, NinjiInfo> player_info;
	std::unordered_map<int, std::unordered_map<int, NinjiGlobalInfo>> player_local_info;
	std::unordered_map<int, LevelBounds> level_bounds;

	int row = 0;
	while(true) {
		int step = sqlite3_step(res);
		if(step == SQLITE_ROW) {
			int data_id = sqlite3_column_int(res, 0);

			if(levels_to_render.count(data_id)) {
				auto pid = std::string((const char*)sqlite3_column_text(res, 1));
				int time = sqlite3_column_int(res, 2);

				int player;
				if(!pid_to_player.count(pid)) {
					pid_to_player[pid] = current_player_index;
					player             = current_player_index;
					current_player_index++;
				} else {
					player = pid_to_player[pid];
				}

				uint8_t* replay_data = (uint8_t*)sqlite3_column_blob(res, 3);
				int replay_size      = sqlite3_column_bytes(res, 3);
				std::vector<uint8_t> decompressed_replay(replay_size);
				gzip_decompress(replay_data, replay_size, decompressed_replay);

				/*
				// Mario, Mario, Luigi, Luigi, Toad
				if(std::unordered_set({ 42177, 42513, 43352, 43581, 43588 }).contains(time)) {
					std::cout << time << ":" << std::endl;

					uint32_t test = *(uint32_t*)&decompressed_replay[0x4];
					toLittleEndian(test);
					std::cout << "    0x4: " << test << std::endl;

					test = *(uint32_t*)&decompressed_replay[0x8];
					toLittleEndian(test);
					std::cout << "    0x8: " << test << std::endl;

					test = *(uint32_t*)&decompressed_replay[0x10];
					toLittleEndian(test);
					std::cout << "    0x10: " << test << std::endl;

					std::cout << "    0x14: " << (int)decompressed_replay[0x14] << std::endl;
					std::cout << "    0x15: " << (int)decompressed_replay[0x15] << std::endl;

					uint16_t test2 = *(uint16_t*)&decompressed_replay[0x16];
					toLittleEndianShort(test2);
					std::cout << "    0x16: " << test2 << std::endl;

					test = *(uint32_t*)&decompressed_replay[0x18];
					toLittleEndian(test);
					std::cout << "    0x18: " << test << std::endl;

					test = *(uint32_t*)&decompressed_replay[0x1C];
					toLittleEndian(test);
					std::cout << "    0x1C: " << test << std::endl;

					test = *(uint32_t*)&decompressed_replay[0x20];
					toLittleEndian(test);
					std::cout << "    0x20: " << test << std::endl;

					test = *(uint32_t*)&decompressed_replay[0x24];
					toLittleEndian(test);
					std::cout << "    0x24: " << test << std::endl;

					test = *(uint32_t*)&decompressed_replay[0x28];
					toLittleEndian(test);
					std::cout << "    0x28: " << test << std::endl;

					test = *(uint32_t*)&decompressed_replay[0x2C];
					toLittleEndian(test);
					std::cout << "    0x2C: " << test << std::endl;

					test = *(uint32_t*)&decompressed_replay[0x30];
					toLittleEndian(test);
					std::cout << "    0x30: " << test << std::endl;

					test = *(uint32_t*)&decompressed_replay[0x34];
					toLittleEndian(test);
					std::cout << "    0x34: " << test << std::endl;
				}*/

				/*
								std::cout << "YUOOOOOOOOOOOOOOOOOOOOO" << std::endl;

								uint32_t test = *(uint32_t*)&decompressed_replay[0x4];
								toLittleEndian(test);
								std::cout << "0x4: " << test << std::endl;

								test = *(uint32_t*)&decompressed_replay[0x8];
								toLittleEndian(test);
								std::cout << "0x8: " << test << std::endl;

								test = *(uint32_t*)&decompressed_replay[0x10];
								toLittleEndian(test);
								std::cout << "0x10: " << test << std::endl;

								std::cout << "0x14: " << (int)decompressed_replay[0x14] << std::endl;
								std::cout << "0x15: " << (int)decompressed_replay[0x15] << std::endl;

								uint16_t test2 = *(uint16_t*)&decompressed_replay[0x16];
								toLittleEndianShort(test2);
								std::cout << "0x16: " << test2 << std::endl;

								test = *(uint32_t*)&decompressed_replay[0x18];
								toLittleEndian(test);
								std::cout << "0x18: " << test << std::endl;

								test = *(uint32_t*)&decompressed_replay[0x1C];
								toLittleEndian(test);
								std::cout << "0x1C: " << test << std::endl;

								test = *(uint32_t*)&decompressed_replay[0x20];
								toLittleEndian(test);
								std::cout << "0x20: " << test << std::endl;

								test = *(uint32_t*)&decompressed_replay[0x24];
								toLittleEndian(test);
								std::cout << "0x24: " << test << std::endl;

								test = *(uint32_t*)&decompressed_replay[0x28];
								toLittleEndian(test);
								std::cout << "0x28: " << test << std::endl;

								test = *(uint32_t*)&decompressed_replay[0x2C];
								toLittleEndian(test);
								std::cout << "0x2C: " << test << std::endl;

								test = *(uint32_t*)&decompressed_replay[0x30];
								toLittleEndian(test);
								std::cout << "0x30: " << test << std::endl;

								test = *(uint32_t*)&decompressed_replay[0x34];
								toLittleEndian(test);
								std::cout << "0x34: " << test << std::endl;

								if(!level_bounds.contains(data_id)) {
									level_bounds[data_id] = LevelBounds {};
								}
								*/

				uint8_t charactor                  = decompressed_replay[0x14];
				player_local_info[data_id][player] = NinjiGlobalInfo { charactor };

				size_t current_offset = 0x3C;
				int i                 = 0;
				while(true) {
					uint8_t flags        = decompressed_replay[current_offset] >> 4;
					uint8_t player_state = decompressed_replay[current_offset] & 0x0F;
					current_offset++;
					uint16_t x = *(uint16_t*)&decompressed_replay[current_offset];
					current_offset += 2;
					uint16_t y = *(uint16_t*)&decompressed_replay[current_offset];
					current_offset += 2;

					if(flags & 0b00000110) {
						uint8_t unk1 = decompressed_replay[current_offset];
						current_offset++;
						ninji_paths[data_id][player].push_back(NinjiFrame { player_state, x, y, (NinjiFrameInfo)unk1 });
						// if(!unique_states.contains(unk1)) {
						//	std::cout << "NEW " << (int)unk1 << std::endl;
						//	unique_states.emplace(unk1);
						// }
					} else {
						ninji_paths[data_id][player].push_back(NinjiFrame { player_state, x, y });
					}

					if(x == 0 && y == 0) {
						// Very likely the last frame, end
						// std::cout << i << std::endl;
						break;
					}

					// std::cout << x << " " << y << std::endl;
					i++;
				}

				if(pid_to_player.size() == 1000) {
					// Break early for testing
					std::cout << "Ending early for testing" << std::endl;
					break;
				}
			}

			row++;

			if(row % 10000 == 0) {
				std::cout << "Handled ninji row " << row << std::endl;
			}
		} else if(step == SQLITE_DONE) {
			break;
		} else if(step == SQLITE_BUSY) {
			// Ignore
		}
	}

	// Obtain player info
	char* player_query = "SELECT name,code,country,mii_image FROM user WHERE pid = ?";
	rc                 = sqlite3_prepare_v2(db, player_query, -1, &res, 0);
	if(rc != SQLITE_OK) {
		std::cout << "Sqlite could not prepare" << std::endl;
		return -1;
	}

	std::vector<std::string> miis_to_download; // JPGs
	std::vector<int> miis_to_download_player;
	std::unordered_map<int, std::string> mii_images;
	std::unordered_set<std::string> used_flags;

	row = 0;
	/*
	for(auto& player : pid_to_player) {
		sqlite3_bind_text(res, 1, player.first.c_str(), player.first.size(), NULL);

		while(true) {
			int step = sqlite3_step(res);
			if(step == SQLITE_ROW) {
				auto name          = std::string((const char*)sqlite3_column_text(res, 0));
				auto code          = std::string((const char*)sqlite3_column_text(res, 1));
				auto country       = std::string((const char*)sqlite3_column_text(res, 2));
				auto mii_image_url = std::string((const char*)sqlite3_column_text(res, 3));

				player_info[player.second] = NinjiInfo { name, code, country };
				miis_to_download.push_back(mii_image_url);
				miis_to_download_player.push_back(player.second);
				used_flags.emplace(country);
			} else if(step == SQLITE_BUSY) {
				// Ignore
			} else if(step == SQLITE_DONE) {
				break;
			}
		}

		row++;

		if(row % 10000 == 0) {
			std::cout << "Handled user row " << row << std::endl;
		}

		sqlite3_reset(res);
		sqlite3_clear_bindings(res);
	}
	*/

	sqlite3_finalize(res);
	sqlite3_close(db);

	// Download all images
	// downloadMiis(miis_to_download, miis_to_download_player, mii_images);
	std::cout << "Downloaded " << mii_images.size() << " images" << std::endl;
	row = 0;
	for(auto& image : mii_images) {
		SkBitmap* bitmap = new SkBitmap();
		std::unique_ptr<SkCodec> jpeg
			= SkCodec::MakeFromData(SkData::MakeWithCopy(image.second.data(), image.second.size()));
		SkImageInfo info = jpeg->getInfo().makeColorType(kBGRA_8888_SkColorType);
		bitmap->allocPixels(info);
		jpeg->getPixels(info, bitmap->getPixels(), bitmap->rowBytes());
		bitmap->setImmutable();
		player_info[image.first].mii_image = bitmap;

		row++;

		if(row % 10000 == 0) {
			std::cout << "Handled bitmap row " << row << std::endl;
		}
	}
	miis_to_download.clear();
	miis_to_download_player.clear();
	mii_images.clear();

	// Create images for players
	std::unordered_map<int, std::unordered_map<int, SkBitmap*>> player_image;
	for(int player = 0; player < 4; player++) {
		std::string player_name;
		switch(player) {
		case 0:
			player_name = "mario";
			break;
		case 1:
			player_name = "luigi";
			break;
		case 2:
			player_name = "toad";
			break;
		case 3:
			player_name = "toadette";
			break;
		}

		// https://github.com/kinnay/Nintendo-File-Formats/wiki/SMM-2-Ninji-Ghosts#player-state
		for(int state = 0; state < 16; state++) {
			// TODO some states have multiple possible states within them (eg walking)
			// Gamestyles also change images
			// 0 (standing, walking, running): column 3 row 2
			// 1 (jumping): column 7 row 2
			// 2 (swimming): column 10 row 2
			// 3 (climbing): column 14 row 2
			// 4 ("hipat" and link down slash): NONE
			// 5 (slipping): column 15 row 2
			// 6 ("wsld"): NONE
			// 7 (clear pipe and dry bones): column 17 row 2 second dry bones
			// 8 (cat attack and clown car): column 19 row 2 clown car in folder
			// 9 (tree top and lakitu cloud): column 19 row 2 cloud in other spritesheet
			// 10 (goomba shoe, koopa troopa, or yoshi): column 19 row 2 yoshi in other spritesheet
			// 11 (walking cat): column 3 row 2
			// 12 (unknown)
			SkBitmap* bitmap = new SkBitmap();
			std::string filename
				= (std::string("../assets/players/") + player_name + "/" + std::to_string(state) + ".png").c_str();
			std::unique_ptr<SkCodec> player_sprite = SkCodec::MakeFromStream(SkStream::MakeFromFile(filename.c_str()));
			SkImageInfo info                       = player_sprite->getInfo().makeColorType(kBGRA_8888_SkColorType);
			bitmap->allocPixels(info);
			player_sprite->getPixels(info, bitmap->getPixels(), bitmap->rowBytes());
			bitmap->setImmutable();
			player_image[player][state] = bitmap;
		}
	}

	// Create images for flags
	std::unordered_map<std::string, SkBitmap*> flag_image;
	for(auto& flag : used_flags) {
		SkBitmap* bitmap                    = new SkBitmap();
		std::unique_ptr<SkCodec> flag_codec = SkCodec::MakeFromStream(
			SkStream::MakeFromFile((std::string("../assets/flags/") + flag + ".png").c_str()));
		SkImageInfo info = flag_codec->getInfo().makeColorType(kBGRA_8888_SkColorType);
		bitmap->allocPixels(info);
		flag_codec->getPixels(info, bitmap->getPixels(), bitmap->rowBytes());
		bitmap->setImmutable();
		flag_image[flag] = bitmap;
	}

	// Create images for levels
	std::unordered_map<int, SkBitmap*> level_overworld_image;
	std::unordered_map<int, SkBitmap*> level_subworld_image;
	for(auto& level : levels_to_render) {
		std::string overworld_path = std::string("../assets/levels/") + std::to_string(level) + ".bcd.overworld.png";
		SkBitmap* overworld_bitmap = new SkBitmap();
		std::unique_ptr<SkCodec> overworld_codec
			= SkCodec::MakeFromStream(SkStream::MakeFromFile(overworld_path.c_str()));
		SkImageInfo info = overworld_codec->getInfo().makeColorType(kBGRA_8888_SkColorType);
		overworld_bitmap->allocPixels(info);
		overworld_codec->getPixels(info, overworld_bitmap->getPixels(), overworld_bitmap->rowBytes());
		overworld_bitmap->setImmutable();
		level_overworld_image[level] = overworld_bitmap;

		std::string subworld_path = std::string("../assets/levels/") + std::to_string(level) + ".bcd.subworld.png";
		if(std::filesystem::exists(subworld_path)) {
			SkBitmap* subworld_bitmap = new SkBitmap();
			std::unique_ptr<SkCodec> subworld_codec
				= SkCodec::MakeFromStream(SkStream::MakeFromFile(subworld_path.c_str()));
			SkImageInfo info2 = subworld_codec->getInfo().makeColorType(kBGRA_8888_SkColorType);
			subworld_bitmap->allocPixels(info2);
			subworld_codec->getPixels(info2, subworld_bitmap->getPixels(), subworld_bitmap->rowBytes());
			subworld_bitmap->setImmutable();
			level_subworld_image[level] = subworld_bitmap;
		}
	}

	// Paint for rendering, preserves pixel art
	SkPaint paint;
	paint.setAntiAlias(false);

	// canvas.drawBitmap(Bitmap, 0, 0, &paint);

	// Start rendering to screen
	uint32_t windowFlags = 0;

	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);

	SDL_GLContext glContext = nullptr;
#if defined(SK_BUILD_FOR_ANDROID) || defined(SK_BUILD_FOR_IOS)
	// For Android/iOS we need to set up for OpenGL ES and we make the window hi res & full screen
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
	windowFlags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_BORDERLESS | SDL_WINDOW_FULLSCREEN_DESKTOP
				  | SDL_WINDOW_ALLOW_HIGHDPI;
#else
	// For all other clients we use the core profile and operate in a window
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);

	windowFlags  = SDL_WINDOW_OPENGL | SDL_WINDOW_ALLOW_HIGHDPI;
#endif
	static const int kStencilBits = 8; // Skia needs 8 stencil bits
	SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
	SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
	SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
	SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
	SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
	SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, kStencilBits);

	SDL_GL_SetAttribute(SDL_GL_ACCELERATED_VISUAL, 1);

	// If you want multisampling, uncomment the below lines and set a sample count
	static const int kMsaaSampleCount = 0; // 4;
	// SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 1);
	// SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, kMsaaSampleCount);

	/*
	 * In a real application you might want to initialize more subsystems
	 */
	if(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
		std::cout << "SDL_Init error" << std::endl;
		return 1;
	}

	// Setup window
	// This code will create a window with the same resolution as the user's desktop.
	SDL_DisplayMode dm;
	if(SDL_GetDesktopDisplayMode(0, &dm) != 0) {
		std::cout << "SDL_GetDesktopDisplayMode error" << std::endl;
		return 1;
	}

	// Override screen dimensions with level image
	// int data_id = *levels_to_render.begin();
	// dm.w        = level_overworld_image[data_id]->width();
	// dm.h        = level_overworld_image[data_id]->height();
	// if(level_subworld_image.contains(data_id)) {
	//	dm.h += level_subworld_image[data_id]->height();
	//}
	// Choose max possible to encompass all
	dm.w = 3840;
	dm.h = 432 + 2688;

	SDL_Window* window
		= SDL_CreateWindow("SDL Window", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, dm.w, dm.h, windowFlags);

	if(!window) {
		std::cout << "SDL_CreateWindow error" << std::endl;
		return 1;
	}

	// Allow dragging window by clicking
	/*
	SDL_SetWindowHitTest(
		window,
		+[](SDL_Window* win, const SDL_Point* area, void* data) -> SDL_HitTestResult { return SDL_HITTEST_DRAGGABLE; },
		nullptr);
		*/

	// To go fullscreen
	// SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN);

	// try and setup a GL context
	glContext = SDL_GL_CreateContext(window);
	if(!glContext) {
		std::cout << "SDL_GL_CreateContext error" << std::endl;
		return 1;
	}

	int success = SDL_GL_MakeCurrent(window, glContext);
	if(success != 0) {
		std::cout << "SDL_GL_MakeCurrent error" << std::endl;
		return success;
	}

	uint32_t windowFormat = SDL_GetWindowPixelFormat(window);
	int contextType;
	SDL_GL_GetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, &contextType);

	int dw, dh;
	SDL_GL_GetDrawableSize(window, &dw, &dh);

	// Before using any OpenGL functions
	gladLoadGL();

	glViewport(0, 0, dw, dh);
	glClearColor(1, 1, 1, 1);
	glClearStencil(0);
	glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

	// setup GrContext
	auto glInterface = GrGLMakeNativeInterface();

	// setup contexts
	sk_sp<GrDirectContext> grContext(GrDirectContext::MakeGL(glInterface));
	SkASSERT(grContext);

	// Wrap the frame buffer object attached to the screen in a Skia render target so Skia can
	// render to it
	GrGLint buffer;
	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &buffer);
	GrGLFramebufferInfo info;
	info.fFBOID = (GrGLuint)buffer;

#if defined(SK_BUILD_FOR_ANDROID)
	info.fFormat = GL_RGB8_OES;
#else
	info.fFormat = GL_RGB8;
#endif

	GrBackendRenderTarget target(dw, dh, kMsaaSampleCount, kStencilBits, info);

	// setup SkSurface
	// To use distance field text, use commented out SkSurfaceProps instead
	// SkSurfaceProps props(SkSurfaceProps::kUseDeviceIndependentFonts_Flag,
	//                      SkSurfaceProps::kLegacyFontHost_InitType);
	SkSurfaceProps props;
	sk_sp<SkSurface> surface(SkSurface::MakeFromBackendRenderTarget(
		grContext.get(), target, kBottomLeft_GrSurfaceOrigin, kRGB_888x_SkColorType, nullptr, &props));

	SkCanvas* canvas = surface->getCanvas();
	canvas->scale((float)dw / dm.w, (float)dh / dm.h);

	// create a surface for CPU rasterization
	sk_sp<SkSurface> cpuSurface(SkSurface::MakeRaster(canvas->imageInfo()));

	// SkCanvas* offscreen = cpuSurface->getCanvas();
	// offscreen->save();
	// offscreen->translate(50.0f, 50.0f);
	// offscreen->drawPath(create_star(), paint);
	// offscreen->restore();
	// sk_sp<SkImage> image = cpuSurface->makeImageSnapshot();

	int rotation = 0;
	SkFont font;
	bool stop = false;
	for(auto data_id : levels_to_render) {
		int frame               = 0;
		int player_update_frame = 0;
		int player_update       = 0;
		while(!stop) {
			canvas->clear(SK_ColorWHITE);

			SDL_Event event;
			while(SDL_PollEvent(&event)) {
				switch(event.type) {
				case SDL_KEYDOWN: {
					SDL_Keycode key = event.key.keysym.sym;
					if(key == SDLK_ESCAPE) {
						stop = true;
					}

					int width;
					int height;
					SDL_GetWindowPosition(window, &width, &height);
					switch(key) {
					case SDLK_RIGHT:
						SDL_SetWindowPosition(window, width - 50, height);
						break;
					case SDLK_LEFT:
						SDL_SetWindowPosition(window, width + 50, height);
						break;
					case SDLK_DOWN:
						SDL_SetWindowPosition(window, width, height - 50);
						break;
					case SDLK_UP:
						SDL_SetWindowPosition(window, width, height + 50);
						break;
					}
					break;
				}
				case SDL_QUIT:
					stop = true;
					break;
				default:
					break;
				}
			}

			canvas->drawImage(level_overworld_image[data_id]->asImage(), 0, 0);
			if(level_subworld_image.contains(data_id)) {
				canvas->drawImage(
					level_subworld_image[data_id]->asImage(), 0, level_overworld_image[data_id]->height());
			}

			// Draw all players
			// canvas->scale()
			int players_rendered = 0;
			for(auto& ninji : ninji_paths[data_id]) {
				if(ninji.second.size() > player_update) {
					// auto& player = player_info[ninji.first];
					auto& player_global = player_local_info[data_id][ninji.first];

					auto& frame = ninji.second[player_update];
					if(ninji_is_subworld[data_id][ninji.first]) {
						canvas->drawImage(player_image[player_global.charactor][frame.state]->asImage(),
							frame.x / 16 - 8 * 13,
							level_subworld_image[data_id]->height() - (frame.y / 16 - 16 * 5)
								+ level_overworld_image[data_id]->height());
					} else {
						canvas->drawImage(player_image[player_global.charactor][frame.state]->asImage(),
							frame.x / 16 - 8 * 13, level_overworld_image[data_id]->height() - (frame.y / 16 - 16 * 5));
					}

					if(frame.info == NinjiFrameInfo::PIPE_SUBWORLD) {
						ninji_is_subworld[data_id][ninji.first] = true;
					}

					if(frame.info == NinjiFrameInfo::PIPE_OVERWORLD) {
						ninji_is_subworld[data_id][ninji.first] = false;
					}

					players_rendered++;

					// std::cout << "Draw player " << player.name << " at " << (frame.x / 16) << " " << (frame.y / 16)
					// << std::endl;
				}
			}

			if(player_update_frame == 1) {
				player_update_frame = 0;
				player_update++;
			}

			canvas->flush();
			SDL_GL_SwapWindow(window);
			frame++;
			player_update_frame++;

			if(players_rendered == 0) {
				break;
			}
		}
	}

	if(glContext) {
		SDL_GL_DeleteContext(glContext);
	}

	// Destroy window
	SDL_DestroyWindow(window);

	// Quit SDL subsystems
	SDL_Quit();

	return 0;
}