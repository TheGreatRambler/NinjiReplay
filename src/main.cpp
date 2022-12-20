#define SK_GL
#include "glad/glad.h"

#define SK_RELEASE
#include <SDL.h>
#include <codec/SkCodec.h>
#include <core/SkBitmap.h>
#include <core/SkCanvas.h>
#include <core/SkColor.h>
#include <core/SkFont.h>
#include <core/SkGraphics.h>
#include <core/SkImage.h>
#include <core/SkSurface.h>
#include <core/SkTextBlob.h>
#include <core/SkTypeface.h>
#include <curl/curl.h>
#include <encode/SkPngEncoder.h>
#include <filesystem>
#include <fstream>
#include <gpu/GrBackendSurface.h>
#include <gpu/GrDirectContext.h>
#include <gpu/gl/GrGLInterface.h>
#include <iostream>
#include <sqlite3.h>
#include <unordered_map>
#include <unordered_set>
#include <utils/SkRandom.h>
#include <zlib.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
}

#ifdef WIN32
#include <Windows.h>
#endif

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

#define RENDER_VIDEO 1
//#define RENDER_SCREEN 1

//#define DRAW_NAMES 1

//#define RENDER_PLAYER 1
#define RENDER_LINES 1

//#define STOP_EARLY 1

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
	std::unordered_map<int, std::string>& mii_images, std::unordered_map<int, std::string>& player_to_pid) {
	// First handle cached
	auto it  = miis_to_download_player.begin();
	auto it2 = miis_to_download.begin();
	while(it != miis_to_download_player.end()) {
		if(std::filesystem::exists(std::string("../mii_cache/") + player_to_pid[*it] + ".png")) {
			std::ifstream mii_image(
				std::string("../mii_cache/") + player_to_pid[*it] + ".png", std::ios::in | std::ios::binary);
			std::string data((std::istreambuf_iterator<char>(mii_image)), std::istreambuf_iterator<char>());
			mii_image.close();
			mii_images[*it] = data;
			it              = miis_to_download_player.erase(it);
			it2             = miis_to_download.erase(it2);
		} else {
			++it;
			++it2;
		}
	}

	if(miis_to_download.size() > 0) {
		CURLM* cm;
		CURLMsg* msg;
		unsigned int transfers = 0;
		int msgs_left          = -1;
		int still_alive        = 1;

		constexpr int MAX_PARALLEL = 50;
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
			curl_easy_setopt(eh, CURLOPT_TIMEOUT, (5 * 60));
			// curl_easy_setopt(eh, CURLOPT_PRIVATE, miis_to_download_player[transfers]);
			curl_multi_add_handle(cm, eh);
		}

		int readRequests = 0;

		do {
			// std::cout << "CURL download loop " << still_alive << " " << transfers << " " << readRequests <<
			// std::endl;
			curl_multi_perform(cm, &still_alive);

			while((msg = curl_multi_info_read(cm, &msgs_left))) {
				if(msg->msg == CURLMSG_DONE) {
					CURL* e = msg->easy_handle;
					// long player;
					// curl_easy_getinfo(msg->easy_handle, CURLINFO_PRIVATE, &player);
					//  fprintf(stderr, "R: %d - %s <%s>\n", msg->data.result, curl_easy_strerror(msg->data.result),
					//  url);
					char* url;
					curl_easy_getinfo(msg->easy_handle, CURLINFO_EFFECTIVE_URL, &url);
					readRequests++;
					std::cout << "Handled " << url << std::endl;
					std::cout << "Read " << readRequests << " requests of " << NUM_URLS << std::endl;
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
					curl_easy_setopt(eh, CURLOPT_TIMEOUT, 60L);
					// curl_easy_setopt(eh, CURLOPT_PRIVATE, miis_to_download_player[transfers]);
					curl_multi_add_handle(cm, eh);
					transfers++;
				}
			}

			if(still_alive) {
				curl_multi_wait(cm, NULL, 0, 1000, NULL);
			}
		} while(readRequests < NUM_URLS);

		for(auto& image : mii_images) {
			if(!std::filesystem::exists(std::string("../mii_cache/") + player_to_pid[image.first] + ".png")) {
				std::filesystem::create_directory("../mii_cache");
				auto cached_mii_image = std::fstream(std::string("../mii_cache/") + player_to_pid[image.first] + ".png",
					std::ios::out | std::ios::binary);
				cached_mii_image.write(image.second.c_str(), image.second.size());
				cached_mii_image.close();
			}
		}

		curl_multi_cleanup(cm);
		curl_global_cleanup();
	}
}

void encode_frame(AVFormatContext* fmt_ctx, AVCodecContext* enc_ctx, AVFrame* frame, AVPacket* pkt, AVStream* stream) {
	int ret;
	/* send the frame to the encoder */
	if(frame)
		printf("Send frame %u (%u s)\n", frame->pts, frame->pts / 60);
	ret = avcodec_send_frame(enc_ctx, frame);
	if(ret < 0) {
		fprintf(stderr, "Error sending a frame for encoding\n");
		char error[MAX_PATH];
		av_strerror(ret, error, MAX_PATH);
		std::cout << error << std::endl;
		exit(1);
	}
	while(ret >= 0) {
		ret = avcodec_receive_packet(enc_ctx, pkt);
		if(ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
			return;
		else if(ret < 0) {
			fprintf(stderr, "Error during encoding\n");
			exit(1);
		}

		av_packet_rescale_ts(pkt, enc_ctx->time_base, stream->time_base);
		pkt->stream_index = stream->index;

		printf("Write packet %3" PRId64 " (size=%5d)\n", pkt->pts, pkt->size);
		/* Write the compressed frame to the media file. */
		ret = av_interleaved_write_frame(fmt_ctx, pkt);
	}
}

int main(int argc, char* argv[]) {
#ifdef WIN32
	SetConsoleOutputCP(CP_UTF8);
#endif

	std::cout << "Starting" << std::endl;

	sqlite3* db;
	char* err_msg = 0;
	sqlite3_stmt* res;

	int rc = sqlite3_open("../ninji_replay.db", &db);

	if(rc != SQLITE_OK) {
		fprintf(stderr, "Cannot open database: %s\n", sqlite3_errmsg(db));
		sqlite3_close(db);
		return 1;
	}

	// Increase cache size for performance
	sqlite3_exec(db, "PRAGMA cache_size=-20000", 0, 0, 0);

	char* replay_query = "SELECT data_id,pid,time,replay FROM ninji";
	rc                 = sqlite3_prepare_v2(db, replay_query, -1, &res, 0);
	if(rc != SQLITE_OK) {
		std::cout << "Sqlite could not prepare query" << std::endl;
		return -1;
	}

	enum NinjiFrameInfo : int8_t {
		NONE       = -1,
		DEATH_UNK1 = 10,
		DOOR       = 11,
	};

	struct __attribute__((packed, aligned(8))) NinjiFrame {
		uint8_t state;
		uint16_t x;
		uint16_t y;
		uint8_t flags;
		// NinjiFrameInfo info = NinjiFrameInfo::NONE;
		// uint8_t flags       = 0;
	};

	struct __attribute__((packed, aligned(8))) NinjiInfo {
		std::string name;
		std::string code;
		std::string country;
		sk_sp<SkImage> mii_image;
	};

	struct __attribute__((packed, aligned(8))) NinjiGlobalInfo {
		// 0: Mario
		// 1: Luigi
		// 2: Toad
		// 3: Toadette
		uint8_t charactor;
	};

	struct __attribute__((packed, aligned(8))) NinjiTime {
		int player;
		int time;
	};

	struct LevelBounds {
		int start_x;
		int start_y;
	};

	static std::unordered_map<int, std::string> gamestyle = {
		{ 33883306, "smb1" },  // good
		{ 29234075, "nsmbu" }, // good
		{ 28460377, "smw" },   // good
		{ 27439231, "smb3" },  // File format broken
		{ 26746705, "smb1" },
		{ 25984384, "smw" },
		{ 25459053, "sm3dw" },
		{ 25045367, "smb1" },
		{ 24477739, "smb3" },
		{ 23738173, "nsmbu" },
		{ 23303835, "smb3" }, // File format broken
		{ 22587491, "sm3dw" },
		{ 21858065, "nsmbu" },
		{ 20182790, "smw" },   // Contains balloon mario, which has a turn angle that needs extra work/sprites
		{ 17110274, "sm3dw" }, // Not done, needs rocket
		{ 15675466, "smb3" },
		{ 14827235, "smw" },
		{ 14328331, "sm3dw" },
		{ 13428950, "nsmbu" },
		{ 12619193, "smb1" },
		{ 12171034, "nsmbu" },
	};

	static std::unordered_map<std::string, int> country_flags = {
		{ "AC", 0 },
		{ "AD", 1 },
		{ "AE", 2 },
		{ "AF", 3 },
		{ "AG", 4 },
		{ "AI", 5 },
		{ "AL", 6 },
		{ "AM", 7 },
		{ "AO", 8 },
		{ "AQ", 9 },
		{ "AR", 10 },
		{ "AS", 11 },
		{ "AT", 12 },
		{ "AU", 13 },
		{ "AW", 14 },
		{ "AX", 15 },
		{ "AZ", 16 },
		{ "BA", 17 },
		{ "BB", 18 },
		{ "BD", 19 },
		{ "BE", 20 },
		{ "BF", 21 },
		{ "BG", 22 },
		{ "BH", 23 },
		{ "BI", 24 },
		{ "BJ", 25 },
		{ "BL", 26 },
		{ "BM", 27 },
		{ "BN", 28 },
		{ "BO", 29 },
		{ "BQ", 30 },
		{ "BR", 31 },
		{ "BS", 32 },
		{ "BT", 33 },
		{ "BV", 34 },
		{ "BW", 35 },
		{ "BY", 36 },
		{ "BZ", 37 },
		{ "CA", 38 },
		{ "CC", 39 },
		{ "CD", 40 },
		{ "CF", 41 },
		{ "CG", 42 },
		{ "CH", 43 },
		{ "CI", 44 },
		{ "CK", 45 },
		{ "CL", 46 },
		{ "CM", 47 },
		{ "CN", 48 },
		{ "CO", 49 },
		{ "CP", 50 },
		{ "CR", 51 },
		{ "CU", 52 },
		{ "CV", 53 },
		{ "CW", 54 },
		{ "CX", 55 },
		{ "CY", 56 },
		{ "CZ", 57 },
		{ "DE", 58 },
		{ "DG", 59 },
		{ "DJ", 60 },
		{ "DK", 61 },
		{ "DM", 62 },
		{ "DO", 63 },
		{ "DZ", 64 },
		{ "EA", 65 },
		{ "EC", 66 },
		{ "EE", 67 },
		{ "EG", 68 },
		{ "EH", 69 },
		{ "ER", 70 },
		{ "ES", 71 },
		{ "ET", 72 },
		{ "EU", 73 },
		{ "FI", 74 },
		{ "FJ", 75 },
		{ "FK", 76 },
		{ "FM", 77 },
		{ "FO", 78 },
		{ "FR", 79 },
		{ "GA", 80 },
		{ "GB", 81 },
		{ "GD", 82 },
		{ "GE", 83 },
		{ "GF", 84 },
		{ "GG", 85 },
		{ "GH", 86 },
		{ "GI", 87 },
		{ "GL", 88 },
		{ "GM", 89 },
		{ "GN", 90 },
		{ "GP", 91 },
		{ "GQ", 92 },
		{ "GR", 93 },
		{ "GS", 94 },
		{ "GT", 95 },
		{ "GU", 96 },
		{ "GW", 97 },
		{ "GY", 98 },
		{ "HK", 99 },
		{ "HM", 100 },
		{ "HN", 101 },
		{ "HR", 102 },
		{ "HT", 103 },
		{ "HU", 104 },
		{ "IC", 105 },
		{ "ID", 106 },
		{ "IE", 107 },
		{ "IL", 108 },
		{ "IM", 109 },
		{ "IN", 110 },
		{ "IO", 111 },
		{ "IQ", 112 },
		{ "IR", 113 },
		{ "IS", 114 },
		{ "IT", 115 },
		{ "JE", 116 },
		{ "JM", 117 },
		{ "JO", 118 },
		{ "JP", 119 },
		{ "KE", 120 },
		{ "KG", 121 },
		{ "KH", 122 },
		{ "KI", 123 },
		{ "KM", 124 },
		{ "KN", 125 },
		{ "KP", 126 },
		{ "KR", 127 },
		{ "KW", 128 },
		{ "KY", 129 },
		{ "KZ", 130 },
		{ "LA", 131 },
		{ "LB", 132 },
		{ "LC", 133 },
		{ "LI", 134 },
		{ "LK", 135 },
		{ "LR", 136 },
		{ "LS", 137 },
		{ "LT", 138 },
		{ "LU", 139 },
		{ "LV", 140 },
		{ "LY", 141 },
		{ "MA", 142 },
		{ "MC", 143 },
		{ "MD", 144 },
		{ "ME", 145 },
		{ "MF", 146 },
		{ "MG", 147 },
		{ "MH", 148 },
		{ "MK", 149 },
		{ "ML", 150 },
		{ "MM", 151 },
		{ "MN", 152 },
		{ "MO", 153 },
		{ "MP", 154 },
		{ "MQ", 155 },
		{ "MR", 156 },
		{ "MS", 157 },
		{ "MT", 158 },
		{ "MU", 159 },
		{ "MV", 160 },
		{ "MW", 161 },
		{ "MX", 162 },
		{ "MY", 163 },
		{ "MZ", 164 },
		{ "NA", 165 },
		{ "NC", 166 },
		{ "NE", 167 },
		{ "NF", 168 },
		{ "NG", 169 },
		{ "NI", 170 },
		{ "NL", 171 },
		{ "NO", 172 },
		{ "NP", 173 },
		{ "NR", 174 },
		{ "NU", 175 },
		{ "NZ", 176 },
		{ "OM", 177 },
		{ "PA", 178 },
		{ "PE", 179 },
		{ "PF", 180 },
		{ "PG", 181 },
		{ "PH", 182 },
		{ "PK", 183 },
		{ "PL", 184 },
		{ "PM", 185 },
		{ "PN", 186 },
		{ "PR", 187 },
		{ "PS", 188 },
		{ "PT", 189 },
		{ "PW", 190 },
		{ "PY", 191 },
		{ "QA", 192 },
		{ "RE", 193 },
		{ "RO", 194 },
		{ "RS", 195 },
		{ "RU", 196 },
		{ "RW", 197 },
		{ "SA", 198 },
		{ "SB", 199 },
		{ "SC", 200 },
		{ "SD", 201 },
		{ "SE", 202 },
		{ "SG", 203 },
		{ "SH", 204 },
		{ "SI", 205 },
		{ "SJ", 206 },
		{ "SK", 207 },
		{ "SL", 208 },
		{ "SM", 209 },
		{ "SN", 210 },
		{ "SO", 211 },
		{ "SR", 212 },
		{ "SS", 213 },
		{ "ST", 214 },
		{ "SV", 215 },
		{ "SX", 216 },
		{ "SY", 217 },
		{ "SZ", 218 },
		{ "TA", 219 },
		{ "TC", 220 },
		{ "TD", 221 },
		{ "TF", 222 },
		{ "TG", 223 },
		{ "TH", 224 },
		{ "TJ", 225 },
		{ "TK", 226 },
		{ "TL", 227 },
		{ "TM", 228 },
		{ "TN", 229 },
		{ "TO", 230 },
		{ "TR", 231 },
		{ "TT", 232 },
		{ "TV", 233 },
		{ "TW", 234 },
		{ "TZ", 235 },
		{ "UA", 236 },
		{ "UG", 237 },
		{ "UM", 238 },
		{ "UN", 239 },
		{ "US", 240 },
		{ "UY", 241 },
		{ "UZ", 242 },
		{ "VA", 243 },
		{ "VC", 244 },
		{ "VE", 245 },
		{ "VG", 246 },
		{ "VI", 247 },
		{ "VN", 248 },
		{ "VU", 249 },
		{ "WF", 250 },
		{ "WS", 251 },
		{ "XK", 252 },
		{ "YE", 253 },
		{ "YT", 254 },
		{ "ZA", 255 },
		{ "ZM", 256 },
		{ "ZW", 257 },
	};

	std::unordered_set<int> levels_to_render = { 12171034 };
	std::unordered_map<int, std::unordered_map<int, std::vector<NinjiFrame>>> ninji_paths;
	std::unordered_map<int, std::unordered_map<int, int>> ninji_times;
	std::unordered_map<int, int> best_ninji_time;
	std::unordered_map<int, int> worst_ninji_time;
	std::unordered_map<int, std::unordered_map<int, bool>> ninji_is_subworld;
	int current_player_index = 0;
	std::unordered_map<std::string, int> pid_to_player;
	std::unordered_map<int, std::string> player_to_pid;
	std::unordered_map<int, NinjiInfo> player_info;
	std::unordered_map<int, std::unordered_map<int, NinjiGlobalInfo>> player_local_info;
	std::unordered_map<int, LevelBounds> level_bounds;
	std::unordered_map<int, std::unordered_map<int, bool>> player_facing;
	std::unordered_map<int, std::vector<NinjiTime>> level_times;
	std::unordered_map<int, int> level_times_size;

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
					pid_to_player[pid]                  = current_player_index;
					player_to_pid[current_player_index] = pid;
					player                              = current_player_index;
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
								*/

				// if(!level_bounds.contains(data_id)) {
				//	level_bounds[data_id] = LevelBounds {};
				// }

				uint8_t charactor                  = decompressed_replay[0x14];
				player_local_info[data_id][player] = NinjiGlobalInfo { charactor };
				uint32_t frames                    = *(uint32_t*)&decompressed_replay[0x10];
				toLittleEndian(frames);
				// Ninji's are rendered every 4 frames, 2 is because the frames is always two less than it should be
				int frames_size = (frames + 2) / 4;

				size_t current_offset = 0x3C;
				for(int i = 0; i < frames_size; i++) {
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
						// ninji_paths[data_id][player].push_back(
						//	NinjiFrame { player_state, x, y, (NinjiFrameInfo)unk1, flags });
						//  if(!unique_states.contains(unk1)) {
						//	std::cout << "NEW " << (int)unk1 << std::endl;
						//	unique_states.emplace(unk1);
						//  }
						//  if(x & 7 != 1) {
						//	uint16_t unk2 = *(uint16_t*)&decompressed_replay[current_offset];
						//	current_offset += 2;
						//	toLittleEndianShort(unk2);
						// }
					} else {
						// ninji_paths[data_id][player].push_back(
						//	NinjiFrame { player_state, x, y, NinjiFrameInfo::NONE, flags });
					}

					ninji_paths[data_id][player].push_back(NinjiFrame { player_state, x, y, flags });
				}

				level_times[data_id].push_back(NinjiTime { player, time });
				ninji_times[data_id][player] = time;

				if(!best_ninji_time.count(data_id)) {
					best_ninji_time[data_id] = time;
				} else {
					if(time < best_ninji_time[data_id]) {
						best_ninji_time[data_id] = time;
					}
				}

				if(!worst_ninji_time.count(data_id)) {
					worst_ninji_time[data_id] = time;
				} else {
					if(time > worst_ninji_time[data_id]) {
						worst_ninji_time[data_id] = time;
					}
				}

#ifdef STOP_EARLY
				if(ninji_paths[data_id].size() == 300) {
					// Break early for testing
					std::cout << "Ending early for testing" << std::endl;
					break;
				}
#endif
			}

			row++;

			if(row % 1000 == 0) {
				std::cout << "Handled ninji row " << row << std::endl;
			}
		} else if(step == SQLITE_DONE) {
			break;
		} else if(step == SQLITE_BUSY) {
			// Ignore
		}
	}

	std::unordered_map<int, std::vector<int>> ninji_paths_sorted;
	for(auto& ninji_times : level_times) {
		std::cout << "Sorting times for " << ninji_times.first << std::endl;
		std::sort(std::begin(ninji_times.second), std::end(ninji_times.second),
			[](const auto& lhs, const auto& rhs) { return lhs.time > rhs.time; });
		level_times_size[ninji_times.first] = ninji_times.second.size();

		for(auto& time : ninji_times.second) {
			ninji_paths_sorted[ninji_times.first].push_back(time.player);
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

		if(row % 1000 == 0) {
			std::cout << "Handled user row " << row << std::endl;
		}

		sqlite3_reset(res);
		sqlite3_clear_bindings(res);
	}

	sqlite3_finalize(res);
	sqlite3_close(db);

	// Download all images
	downloadMiis(miis_to_download, miis_to_download_player, mii_images, player_to_pid);
	std::cout << "Downloaded " << mii_images.size() << " images" << std::endl;
	row = 0;
	for(auto& image : mii_images) {
		SkBitmap bitmap;
		std::unique_ptr<SkCodec> jpeg
			= SkCodec::MakeFromData(SkData::MakeWithCopy(image.second.data(), image.second.size()));

		if(!jpeg) {
			std::cout << "No Mii image seen at " << row << std::endl;
			row++;
			continue;
		}

		SkImageInfo info = jpeg->getInfo().makeColorType(kBGRA_8888_SkColorType);
		bitmap.allocPixels(info);
		jpeg->getPixels(info, bitmap.getPixels(), bitmap.rowBytes());
		bitmap.setImmutable();

		sk_sp<SkSurface> rasterSurface = SkSurface::MakeRasterN32Premul(24, 24);
		rasterSurface->getCanvas()->drawImageRect(bitmap.asImage(), SkRect::MakeLTRB(75, 75, 512 - 75, 512 - 75),
			SkRect::MakeWH(24, 24), SkSamplingOptions(SkFilterMode::kNearest), nullptr,
			SkCanvas::kStrict_SrcRectConstraint);

		player_info[image.first].mii_image = rasterSurface->makeImageSnapshot();

		row++;

		if(row % 10000 == 0) {
			std::cout << "Handled mii row " << row << std::endl;
		}
	}

	std::cout << "Handled all Miis" << std::endl;

	miis_to_download.clear();
	miis_to_download_player.clear();
	mii_images.clear();

	std::cout << "Cleared Mii vectors" << std::endl;

#ifdef RENDER_PLAYER
	// Create images for players
	std::unordered_map<int, std::unordered_map<int, std::unordered_map<int, sk_sp<SkImage>>>> player_image;
	std::unordered_map<int, std::unordered_map<int, std::unordered_map<int, sk_sp<SkImage>>>> player_mirrored_image;
	for(auto data_id : levels_to_render) {
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
				SkBitmap* bitmap     = new SkBitmap();
				std::string filename = std::string("../assets/players/") + player_name + "/" + gamestyle[data_id] + "/"
									   + std::to_string(state) + ".png";
				if(!std::filesystem::exists(filename)) {
					filename = std::string("../assets/players/") + player_name + "/" + std::to_string(state) + ".png";
				}
				std::unique_ptr<SkCodec> player_sprite
					= SkCodec::MakeFromStream(SkStream::MakeFromFile(filename.c_str()));
				SkImageInfo info = player_sprite->getInfo().makeColorType(kBGRA_8888_SkColorType);
				bitmap->allocPixels(info);
				player_sprite->getPixels(info, bitmap->getPixels(), bitmap->rowBytes());
				bitmap->setImmutable();
				player_image[data_id][player][state] = bitmap->asImage();

				sk_sp<SkSurface> rasterSurface = SkSurface::MakeRasterN32Premul(bitmap->width(), bitmap->height());
				SkCanvas* rasterCanvas         = rasterSurface->getCanvas();
				rasterCanvas->translate(bitmap->width(), 0);
				rasterCanvas->scale(-1, 1);
				rasterCanvas->drawImage(bitmap->asImage(), 0, 0);
				player_mirrored_image[data_id][player][state] = rasterSurface->makeImageSnapshot();
			}
		}
	}

	std::cout << "Created player images" << std::endl;
#endif

	// Create images for flags
	std::unordered_map<std::string, sk_sp<SkImage>> flag_image;
	for(auto& flag : used_flags) {
		SkBitmap* bitmap                    = new SkBitmap();
		std::unique_ptr<SkCodec> flag_codec = SkCodec::MakeFromStream(
			SkStream::MakeFromFile((std::string("../assets/flags/") + flag + ".png").c_str()));
		SkImageInfo info = flag_codec->getInfo().makeColorType(kBGRA_8888_SkColorType);
		bitmap->allocPixels(info);
		flag_codec->getPixels(info, bitmap->getPixels(), bitmap->rowBytes());
		bitmap->setImmutable();

		sk_sp<SkSurface> rasterSurface = SkSurface::MakeRasterN32Premul(36, 24);
		rasterSurface->getCanvas()->drawImageRect(bitmap->asImage(), SkRect::MakeWH(180, 120), SkRect::MakeWH(36, 24),
			SkSamplingOptions(SkFilterMode::kNearest), nullptr, SkCanvas::kStrict_SrcRectConstraint);

		flag_image[flag] = rasterSurface->makeImageSnapshot();
	}

	std::cout << "Created flag images" << std::endl;

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

	std::cout << "Created level images" << std::endl;

	// Paint for rendering, preserves pixel art
	SkPaint paint;
	paint.setAntiAlias(false);
	paint.setColor(SK_ColorWHITE);

	int leaderboard_x_offset   = 3840;
	int leaderboard_width      = 352;
	int leaderboard_height     = 1080;
	int countries_graph_height = 500;

#ifdef RENDER_SCREEN
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
	dm.w = leaderboard_x_offset + leaderboard_width;
	dm.h = 432 + 2688 + countries_graph_height;

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

	SDL_SetWindowPosition(window, 0, 0);
#endif

#ifdef RENDER_VIDEO
	uint8_t endcode[] = { 0, 0, 1, 0xb7 };

	const AVCodec* codec = avcodec_find_encoder_by_name("libx264rgb");
	if(!codec) {
		fprintf(stderr, "Codec not found\n");
		exit(1);
	}

	std::vector<uint8_t> pixelMemory;
#endif

	SkFont nameFont(SkTypeface::MakeFromFile("../assets/fonts/NotoSansJP-Regular.otf"));
	nameFont.setSize(15);
	SkFont rankFont(SkTypeface::MakeFromFile("../assets/fonts/NotoSansJP-Regular.otf"));
	rankFont.setSize(20);
	SkFont countryCountFont(SkTypeface::MakeFromFile("../assets/fonts/NotoSansJP-Regular.otf"));
	rankFont.setSize(10);
	SkFont hoverNameFont(SkTypeface::MakeFromFile("../assets/fonts/NotoSansJP-Regular.otf"));
	hoverNameFont.setSize(10);

	SkPaint hoverNamePaint;
	hoverNamePaint.setColor(SK_ColorWHITE);
	hoverNamePaint.setAlpha(150);

	std::cout << "Prepare to render" << std::endl;

	for(auto data_id : levels_to_render) {
		std::cout << "Start render for " << data_id << std::endl;

		int frame                  = 0;
		int player_update          = 0;
		int player_update_subframe = 0;
		bool stop                  = false;
		// Graph goes under this
		int levels_height = 0;
		if(level_subworld_image.contains(data_id)) {
			levels_height = level_overworld_image[data_id]->height() + level_subworld_image[data_id]->height();
		} else {
			levels_height = level_overworld_image[data_id]->height();
		}

		// Show percent of countries so far
		std::unordered_map<std::string, int> countries_so_far;

#ifdef RENDER_VIDEO
		int width       = leaderboard_x_offset + leaderboard_width;
		int temp_height = levels_height + countries_graph_height;
		// To render leaderboard, must include extra
		int height       = (temp_height < leaderboard_height) ? leaderboard_height : temp_height;
		SkImageInfo info = SkImageInfo::Make(width, height, kRGBA_8888_SkColorType, kPremul_SkAlphaType);
		size_t rowBytes  = info.minRowBytes();
		pixelMemory.resize(rowBytes * height);
		sk_sp<SkSurface> surface = SkSurface::MakeRasterDirect(info, pixelMemory.data(), rowBytes);
		SkCanvas* canvas         = surface->getCanvas();

		std::cout << "Created surface for " << data_id << std::endl;

		AVCodecContext* codec_context = avcodec_alloc_context3(codec);
		if(!codec_context) {
			fprintf(stderr, "Could not allocate video codec context\n");
			exit(1);
		}

		std::cout << "Created codec context for " << data_id << std::endl;

		/* resolution must be a multiple of two */
		codec_context->width  = width;
		codec_context->height = height;
		/* frames per second */
		codec_context->time_base = (AVRational) { 1, 60 };
		codec_context->framerate = (AVRational) { 60, 1 };
		/* emit one intra frame every ten frames
		 * check frame pict_type before passing frame
		 * to encoder, if frame->pict_type is AV_PICTURE_TYPE_I
		 * then gop_size is ignored and the output of encoder
		 * will always be I frame irrespective to gop_size
		 */
		codec_context->gop_size     = 10;
		codec_context->max_b_frames = 1;
		codec_context->pix_fmt      = AV_PIX_FMT_RGB24;
		codec_context->bit_rate     = 8e+7;
		av_opt_set(codec_context->priv_data, "crf", "17", 0);
		av_opt_set(codec_context->priv_data, "preset", "veryslow", 0);

		/* open it */
		if(avcodec_open2(codec_context, codec, NULL) < 0) {
			fprintf(stderr, "Could not open codec\n");
			exit(1);
		}

		AVFrame* video_frame = av_frame_alloc();
		if(!video_frame) {
			fprintf(stderr, "Could not allocate video frame\n");
			exit(1);
		}
		video_frame->format = codec_context->pix_fmt;
		video_frame->width  = codec_context->width;
		video_frame->height = codec_context->height;

		std::cout << "Created video frame for " << data_id << std::endl;

		/* the image can be allocated by any means and av_image_alloc() is
		 * just the most convenient way if av_malloc() is to be used */
		av_frame_get_buffer(video_frame, 32);

		std::cout << "Created av frame for " << data_id << std::endl;

		AVPacket* pkt = av_packet_alloc();

		std::cout << "Created packet for " << data_id << std::endl;

#ifdef DRAW_NAMES
		std::string filename = std::to_string(data_id) + "_names.mp4";
#elif defined(RENDER_LINES)
		std::string filename = std::to_string(data_id) + "_lines.mp4";
#else
		std::string filename = std::to_string(data_id) + ".mp4";
#endif

		AVFormatContext* oc;
		avformat_alloc_output_context2(&oc, NULL, NULL, filename.c_str());
		const AVOutputFormat* fmt = oc->oformat;

		std::cout << "Created output context for " << data_id << std::endl;

		AVStream* stream = avformat_new_stream(oc, NULL);
		avcodec_parameters_from_context(stream->codecpar, codec_context);
		stream->time_base = (AVRational) { 1, 60 };

		std::cout << "Created stream for " << data_id << std::endl;

		avio_open(&oc->pb, filename.c_str(), AVIO_FLAG_WRITE);
		avformat_write_header(oc, NULL);

		std::cout << "Opened output video file " << data_id << std::endl;
#endif

		std::unordered_set<int> seen_states;
		while(!stop) {
			canvas->clear(SK_ColorBLACK);

#ifdef RENDER_SCREEN
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
#endif

			canvas->drawImage(level_overworld_image[data_id]->asImage(), 0, 0);
			if(level_subworld_image.contains(data_id)) {
				canvas->drawImage(
					level_subworld_image[data_id]->asImage(), 0, level_overworld_image[data_id]->height());
			}

			// Draw both "greenscreens" for chromakey
			SkPaint greenscreenPaint;
			greenscreenPaint.setColor(SkColorSetARGB(255, 190, 3, 253));
			canvas->drawRect(
				SkRect::MakeXYWH(leaderboard_x_offset, 0, leaderboard_width, leaderboard_height), greenscreenPaint);
			canvas->drawRect(
				SkRect::MakeXYWH(0, levels_height, leaderboard_x_offset + leaderboard_width, countries_graph_height),
				greenscreenPaint);

			// Draw leaderboard
			for(int rank = 0; rank < 36; rank++) {
				int index = level_times[data_id].size() - 1 - rank;
				if(index <= 0)
					break;

				auto& time   = level_times[data_id][index];
				auto& player = player_info[time.player];

				int y                  = (rank + 1) * 36;
				std::string rankString = std::to_string(level_times_size[data_id] - index);
				canvas->drawSimpleText(rankString.c_str(), rankString.size(), SkTextEncoding::kUTF8,
					leaderboard_x_offset, y, rankFont, paint);
				if(player.mii_image) {
					canvas->drawImage(player.mii_image, leaderboard_x_offset + 46, y - 20);
				}
				canvas->drawImage(flag_image[player.country], leaderboard_x_offset + 76, y - 20);
				canvas->drawSimpleText(player.name.c_str(), player.name.size(), SkTextEncoding::kUTF8,
					leaderboard_x_offset + 116, y, nameFont, paint);
			}

			// Draw countries graph
			std::vector<std::pair<std::string, int>> sorted_country_counts;
			for(auto& entry : countries_so_far) {
				sorted_country_counts.push_back(std::make_pair(entry.first, entry.second));
			}

			std::sort(std::begin(sorted_country_counts), std::end(sorted_country_counts),
				[](const auto& lhs, const auto& rhs) { return lhs.second > rhs.second; });

			int biggest_size = 0;
			int total_height = countries_graph_height - 45;
			for(int i = 0; i < sorted_country_counts.size(); i++) {
				auto& entry = sorted_country_counts[i];
				if(i == 0) {
					biggest_size = entry.second;
				}

				int startX = i * 54;
				canvas->drawImage(flag_image[entry.first], startX + 9, levels_height + countries_graph_height - 24);
				std::string numString = std::to_string(entry.second);
				canvas->drawSimpleText(numString.c_str(), numString.size(), SkTextEncoding::kUTF8, startX + 7,
					levels_height + countries_graph_height - 30, countryCountFont, paint);

				// Draw graph bar
				int bar_height = total_height * ((float)entry.second / biggest_size);
				SkPaint barPaint;
				barPaint.setColor(SK_ColorWHITE);
				canvas->drawRect(SkRect::MakeXYWH((float)startX + 9.0f,
									 (float)(levels_height + total_height - bar_height), 36.0f, (float)bar_height),
					barPaint);
			}

			// Draw all players
			// canvas->scale()
			int players_rendered = 0;
			for(auto player_num : ninji_paths_sorted[data_id]) {
				auto& frames = ninji_paths[data_id][player_num];
#ifdef RENDER_PLAYER
				if(player_update < frames.size()) {
					auto& player                  = player_info[player_num];
					auto& player_local            = player_local_info[data_id][player_num];
					auto& player_sprites          = player_image[data_id][player_local.charactor];
					auto& player_mirrored_sprites = player_mirrored_image[data_id][player_local.charactor];

					auto& frame = frames[player_update];
					// Check that not in pipe transition, very glitchy
					if(!(frame.flags & 0b00000100)) {
						ninji_is_subworld[data_id][player_num] = frame.flags & 0b00001000;

						int x;
						int y;
						if((player_update + 1) < frames.size()) {
							// Lerp
							auto& frame_after = frames[player_update + 1];
							int x_before;
							int y_before;
							int x_after;
							int y_after;
							if(ninji_is_subworld[data_id][player_num]) {
								x_before = frame.x / 16 - 8 * 13;
								x_after  = frame_after.x / 16 - 8 * 13;
								y_before = level_subworld_image[data_id]->height() - (frame.y / 16 - 16 * 6)
										   + level_overworld_image[data_id]->height();
								y_after = level_subworld_image[data_id]->height() - (frame_after.y / 16 - 16 * 6)
										  + level_overworld_image[data_id]->height();
							} else {
								x_before = frame.x / 16 - 8 * 13;
								x_after  = frame_after.x / 16 - 8 * 13;
								y_before = level_overworld_image[data_id]->height() - (frame.y / 16 - 16 * 6);
								y_after  = level_overworld_image[data_id]->height() - (frame_after.y / 16 - 16 * 6);
							}

							float lerp = player_update_subframe / 4.0;
							x          = x_before + lerp * (x_after - x_before);
							y          = y_before + lerp * (y_after - y_before);
						} else {
							if(ninji_is_subworld[data_id][player_num]) {
								x = frame.x / 16 - 8 * 13;
								y = level_subworld_image[data_id]->height() - (frame.y / 16 - 16 * 6)
									+ level_overworld_image[data_id]->height();
							} else {
								x = frame.x / 16 - 8 * 13;
								y = level_overworld_image[data_id]->height() - (frame.y / 16 - 16 * 6);
							}
						}

						if(player_update != 0 && frames[player_update - 1].x != frames[player_update].x) {
							player_facing[data_id][player_num] = frames[player_update].x < frames[player_update - 1].x;
						}

						if(player_facing[data_id][player_num]) {
							auto sprite = player_mirrored_sprites[frame.state];
							canvas->drawImage(sprite, x, y - sprite->height());
						} else {
							auto sprite = player_sprites[frame.state];
							canvas->drawImage(sprite, x, y - sprite->height());
						}

#ifdef DRAW_NAMES
						canvas->drawSimpleText(player.name.c_str(), player.name.size(), SkTextEncoding::kUTF8, x + 16,
							y - 4, hoverNameFont, hoverNamePaint);
#endif
					}

					players_rendered++;

					// std::cout << "Draw player " << player.name << " at " << (frame.x / 16) << " " << (frame.y / 16)
					// << std::endl;
				} else if(player_update == frames.size() && player_update_subframe == 0) {
					// Remove from rankings
					auto size     = level_times[data_id].size();
					auto& last    = level_times[data_id][size - 1];
					auto& country = player_info[last.player].country;
					if(!countries_so_far.contains(country)) {
						countries_so_far[country] = 1;
					} else {
						countries_so_far[country]++;
					}
					level_times[data_id].pop_back();
				}
#endif

#ifdef RENDER_LINES
				SkPaint linePaint;

				// TODO consider something other than linear interpolation
				double percentage = (double)(ninji_times[data_id][player_num] - best_ninji_time[data_id])
									/ (double)(worst_ninji_time[data_id] - best_ninji_time[data_id]);
				constexpr double curve_constant = 0.005;
				double exponential_percentage   = std::pow(1.0 - percentage, 1 / curve_constant - 1);
				SkScalar lineHSV[3];
				lineHSV[0] = 0.0 + exponential_percentage * 120.0;
				lineHSV[1] = 1.0;
				lineHSV[2] = 0.75;
				linePaint.setColor(SkHSVToColor(lineHSV));

				linePaint.setAlpha(200);
				linePaint.setStrokeWidth(1);
				linePaint.setAntiAlias(false);

				if(player_update == frames.size() && player_update_subframe == 0) {
					// Remove from rankings
					auto size     = level_times[data_id].size();
					auto& last    = level_times[data_id][size - 1];
					auto& country = player_info[last.player].country;
					if(!countries_so_far.contains(country)) {
						countries_so_far[country] = 1;
					} else {
						countries_so_far[country]++;
					}
					level_times[data_id].pop_back();
				} else {
					players_rendered++;
				}

				if(player_update != 0 || player_update_subframe != 0) {
					int max_update;
					if(player_update == 0) {
						max_update = 0;
					} else if((player_update + 1) < frames.size()) {
						max_update = player_update;
					} else {
						max_update = frames.size() - 1;
					}

					for(int i = 0; i < max_update; i++) {
						auto& frame = frames[i];
						int x;
						int y;
						bool is_in_subworld = frame.flags & 0b00001000;
						if(is_in_subworld) {
							x = frame.x / 16 - 8 * 13;
							y = level_subworld_image[data_id]->height() - (frame.y / 16 - 16 * 6)
								+ level_overworld_image[data_id]->height();
						} else {
							x = frame.x / 16 - 8 * 13;
							y = level_overworld_image[data_id]->height() - (frame.y / 16 - 16 * 6);
						}

						auto& frame_next = frames[i + 1];
						int x_next;
						int y_next;
						is_in_subworld = frame_next.flags & 0b00001000;
						if(is_in_subworld) {
							x_next = frame_next.x / 16 - 8 * 13;
							y_next = level_subworld_image[data_id]->height() - (frame_next.y / 16 - 16 * 6)
									 + level_overworld_image[data_id]->height();
						} else {
							x_next = frame_next.x / 16 - 8 * 13;
							y_next = level_overworld_image[data_id]->height() - (frame_next.y / 16 - 16 * 6);
						}

						if(!(frame_next.flags & 0b00000100)) {
							canvas->drawLine(
								SkPoint::Make(x + 8, y - 8), SkPoint::Make(x_next + 8, y_next - 8), linePaint);
						}
					}

					if((player_update + 1) < frames.size()) {
						auto& frame                            = frames[player_update];
						ninji_is_subworld[data_id][player_num] = frame.flags & 0b00001000;

						// Lerp
						auto& frame_after = frames[player_update + 1];
						int x_before;
						int y_before;
						int x_after;
						int y_after;
						if(ninji_is_subworld[data_id][player_num]) {
							x_before = frame.x / 16 - 8 * 13;
							x_after  = frame_after.x / 16 - 8 * 13;
							y_before = level_subworld_image[data_id]->height() - (frame.y / 16 - 16 * 6)
									   + level_overworld_image[data_id]->height();
							y_after = level_subworld_image[data_id]->height() - (frame_after.y / 16 - 16 * 6)
									  + level_overworld_image[data_id]->height();
						} else {
							x_before = frame.x / 16 - 8 * 13;
							x_after  = frame_after.x / 16 - 8 * 13;
							y_before = level_overworld_image[data_id]->height() - (frame.y / 16 - 16 * 6);
							y_after  = level_overworld_image[data_id]->height() - (frame_after.y / 16 - 16 * 6);
						}

						float lerp = player_update_subframe / 4.0;
						int x      = x_before + lerp * (x_after - x_before);
						int y      = y_before + lerp * (y_after - y_before);

						canvas->drawLine(
							SkPoint::Make(x_before + 8, y_before - 8), SkPoint::Make(x + 8, y - 8), linePaint);
					}
				}
#endif
			}

			canvas->flush();

			if(player_update_subframe == 3) {
				player_update_subframe = 0;
				player_update++;
			} else {
				player_update_subframe++;
			}

			frame++;

			if(frame % 1000 == 0) {
				std::cout << "Rendered frame " << frame << std::endl;
			}

#ifdef STOP_EARLY
			if(frame == 2000) {
				stop = true;
			}
#endif

			if(players_rendered == 0) {
				std::cout << "Finished " << data_id << std::endl;
				stop = true;
			}

#ifdef RENDER_VIDEO
			// if(!surface->readPixels(info, &video_frame->data[0], rowBytes, 0, 0)) {
			//	std::cout << "Could not write frame to video" << std::endl;
			// }
			for(int y = 0; y < height; y++) {
				for(int x = 0; x < width; x++) {
					memcpy(&video_frame->data[0][(y * width + x) * 3], &pixelMemory[(y * width + x) * 4], 3);
				}
			}
			// memcpy(&video_frame->data[0], pixelMemory.data(), pixelMemory.size());
			video_frame->pts = frame;

			encode_frame(oc, codec_context, video_frame, pkt, stream);

			// if(frame == 100) {
			//	std::cout << "Finished early for render " << data_id << std::endl;
			//	stop = true;
			// }
#endif

#ifdef RENDER_SCREEN
			SDL_GL_SwapWindow(window);
#endif
		}

#ifdef RENDER_VIDEO
		encode_frame(oc, codec_context, NULL, pkt, stream);

		av_write_trailer(oc);
		avio_closep(&oc->pb);
		av_packet_free(&pkt);
		av_freep(&video_frame->data[0]);
		avformat_free_context(oc);
		avcodec_close(codec_context);
#endif
	}

	std::cout << "Finished all levels" << std::endl;

#ifdef RENDER_VIDEO
#endif

#ifdef RENDER_SCREEN
	if(glContext) {
		SDL_GL_DeleteContext(glContext);
	}

	// Destroy window
	SDL_DestroyWindow(window);

	// Quit SDL subsystems
	SDL_Quit();
#endif

	return 0;
}