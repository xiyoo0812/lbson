#pragma once

#include "lua_kit.h"

using namespace std;
using namespace luakit;

#define PHEX(v,c) { char tmp = (char) c; if (tmp >= '0' && tmp <= '9') { v = tmp-'0'; } else { v = tmp - 'a' + 10; } }

//https://bsonspec.org/spec.html
namespace lbson {
    const uint8_t max_bson_depth    = 64;
    const uint32_t max_bson_index   = 1024;

    const uint32_t OP_MSG_CODE      = 2013;
    const uint32_t OP_MSG_HLEN      = 4 * 5 + 1;
    const uint32_t OP_CHECKSUM      = 1 << 0;
    const uint32_t OP_MORE_COME     = 1 << 1;

    static char bson_numstrs[max_bson_index][4];
    static int bson_numstr_len[max_bson_index];

    enum class bson_type : uint8_t {
        BSON_EOO        = 0,
        BSON_REAL       = 1,
        BSON_STRING     = 2,
        BSON_DOCUMENT   = 3,
        BSON_ARRAY      = 4,
        BSON_BINARY     = 5,
        BSON_UNDEFINED  = 6,    //Deprecated
        BSON_OBJECTID   = 7,
        BSON_BOOLEAN    = 8,
        BSON_DATE       = 9,
        BSON_NULL       = 10,
        BSON_REGEX      = 11,
        BSON_DBPOINTER  = 12,   //Deprecated
        BSON_JSCODE     = 13,
        BSON_SYMBOL     = 14,   //Deprecated
        BSON_CODEWS     = 15,   //Deprecated
        BSON_INT32      = 16,
        BSON_TIMESTAMP  = 17,   //special timestamp type only for internal MongoDB use
        BSON_INT64      = 18,
        BSON_INT128     = 19,
        BSON_MINKEY     = 255,
        BSON_MAXKEY     = 127,
    };

    class mgocodec;
    class bson {
    public:
        friend mgocodec;
        slice* encode_slice(lua_State* L) {
            m_buff->clean();
            pack_dict(L, 0);
            return m_buff->get_slice();
        }

        int encode(lua_State* L) {
            size_t data_len = 0;
            slice* slice = encode_slice(L);
            const char* data = (const char*)slice->data(&data_len);
            lua_pushlstring(L, data, data_len);
            return 1;
        }

        int decode(lua_State* L) {
            m_buff->clean();
            size_t data_len = 0;
            const char* buf = lua_tolstring(L, 1, &data_len);
            if (data_len > 0) m_buff->push_data((uint8_t*)buf, data_len);
            return decode_slice(L, m_buff->get_slice());
        }

        int decode_slice(lua_State* L, slice* slice) {
            lua_settop(L, 0);
            try {
                unpack_dict(L, slice, false);
            } catch (const exception& e){
                luaL_error(L, e.what());
            }
            return lua_gettop(L);
        }

        uint8_t* encode_pairs(lua_State* L, size_t* data_len) {
            int n = lua_gettop(L);
            if (n < 2 || n % 2 != 0) {
                luaL_error(L, "Invalid ordered dict");
            }
            size_t sz;
            size_t offset = m_buff->size();
            m_buff->write<uint32_t>(0);
            for (int i = 0; i < n; i += 2) {
                int vt = lua_type(L, i + 2);
                if (vt != LUA_TNIL && vt != LUA_TNONE) {
                    const char* key = lua_tolstring(L, i + 1, &sz);
                    if (key == nullptr) {
                        luaL_error(L, "Argument %d need a string", i + 1);
                    }
                    lua_pushvalue(L, i + 2);
                    pack_one(L, key, sz, 0);
                    lua_pop(L, 1);
                }
            }
            m_buff->write<uint8_t>(0);
            uint32_t size = m_buff->size() - offset;
            m_buff->copy(offset, (uint8_t*)&size, sizeof(uint32_t));
            //返回结果
            return m_buff->data(data_len);
        }

        void set_buff(luabuf* buf) {
            m_buff = buf;
        }

        int date(lua_State* L, int64_t value) {
            return make_bson_value(L, bson_type::BSON_DATE, (uint8_t*)&value, sizeof(value));
        }

        int int64(lua_State* L, int64_t value) {
            return make_bson_value(L, bson_type::BSON_INT64, (uint8_t*)&value, sizeof(value));
        }

        int objectid(lua_State* L) {
            size_t data_len = 0;
            const char* value = lua_tolstring(L, 1, &data_len);
            if (data_len != 24) return luaL_error(L, "Invalid object id");
            char buffer[16] = { 0 };
            write_objectid(L, buffer, value);
            return make_bson_value(L, bson_type::BSON_OBJECTID, (uint8_t*)buffer, 12);
        }

        int pairs(lua_State* L) {
            m_buff->clean();
            size_t data_len = 0;
            m_buff->write<uint8_t>(0);
            m_buff->write<uint8_t>((uint8_t)bson_type::BSON_DOCUMENT);
            uint8_t* data = encode_pairs(L, &data_len);
            lua_pushlstring(L, (const char*)data, data_len);
            return 1;
        }

        int binary(lua_State* L) {
            m_buff->clean();
            size_t data_len = 0;
            uint8_t* value = (uint8_t*)lua_tolstring(L, 1, &data_len);
            m_buff->write<uint8_t>(0);
            m_buff->write<uint8_t>((uint8_t)bson_type::BSON_BINARY);
            m_buff->write<uint8_t>(0); //subtype
            m_buff->write<int32_t>(data_len);
            if (data_len > 0) m_buff->push_data(value, data_len);
            lua_pushlstring(L, (const char*)m_buff->head(), m_buff->size());
            return 1;
        }

        int regex(lua_State* L) {
            m_buff->clean();
            size_t data_len = 0;
            m_buff->write<uint8_t>(0);
            m_buff->write<uint8_t>((uint8_t)bson_type::BSON_REGEX);
            uint8_t* val1 = (uint8_t*)lua_tolstring(L, 1, &data_len);
            m_buff->push_data(val1, data_len);
            m_buff->write<uint8_t>(0);
            uint8_t* val2 = (uint8_t*)lua_tolstring(L, 2, &data_len);
            m_buff->push_data(val2, data_len);
            m_buff->write<uint8_t>(0);
            lua_pushlstring(L, (const char*)m_buff->head(), m_buff->size());
            return 1;
        }

    protected:
        int make_bson_value(lua_State *L, bson_type type, uint8_t* value, size_t len) {
            m_buff->clean();
            m_buff->write<uint8_t>(0);
            m_buff->write<uint8_t>((uint8_t)type);
            m_buff->push_data(value, len);
            lua_pushlstring(L, (const char*)m_buff->head(), m_buff->size());
            return 1;
        }

        size_t bson_index(char* str, size_t i) {
            if (i < max_bson_index) {
                memcpy(str, bson_numstrs[i], 4);
                return bson_numstr_len[i];
            }
            return sprintf(str, "%zd", i);
        }

        void pack_date(lua_State* L) {
            lua_getfield(L, -1, "date");
            m_buff->write<uint64_t>(lua_tointeger(L, -1) * 1000);
            lua_pop(L, 1);
        }

        void pack_int64(lua_State* L) {
            lua_getfield(L, -1, "value");
            m_buff->write<uint64_t>(lua_tointeger(L, -1));
            lua_pop(L, 1);
        }

        void pack_string(lua_State* L) {
            size_t data_len;
            lua_getfield(L, -1, "value");
            const char* data = lua_tolstring(L, -1, &data_len);
            m_buff->push_data((uint8_t*)data, data_len);
            lua_pop(L, 1);
        }

        void pack_objectid(lua_State* L) {
            size_t data_len;
            lua_getfield(L, -1, "objid");
            const char* data = lua_tolstring(L, -1, &data_len);
            if (data_len != 24) luaL_error(L, "Invalid object id");
            char buffer[16] = { 0 };
            write_objectid(L, buffer, data);
            m_buff->push_data((uint8_t*)buffer, 12);
            lua_pop(L, 1);
        }

        void pack_binary(lua_State* L) {
            lua_guard g(L);
            size_t bin_len;
            lua_getfield(L, -1, "binary");
            const char* bin = lua_tolstring(L, -1, &bin_len);
            lua_getfield(L, -2, "subtype");
            m_buff->write<uint32_t>(bin_len);
            m_buff->write<uint8_t>(lua_tointeger(L, -1));
            m_buff->push_data((uint8_t*)bin, bin_len);
        }

        void pack_regex(lua_State* L) {
            lua_guard g(L);
            size_t regex_len;
            lua_getfield(L, -1, "pattern");
            const char* pattern = lua_tolstring(L, -1, &regex_len);
            write_cstring(pattern, regex_len);
            lua_getfield(L, -2, "option");
            const char* option = lua_tolstring(L, -1, &regex_len);
            write_cstring(option, regex_len);
        }
        
        void write_cstring(const char* buf, size_t len) {
            if (len > 0) m_buff->push_data((uint8_t*)buf, len);
            m_buff->write<char>('\0');
        }

        void write_string(const char* buf, size_t len) {
            m_buff->write<uint32_t>(len + 1);
            write_cstring(buf, len);
        }

        void write_key(bson_type type, const char* key, size_t klen) {
            m_buff->write<uint8_t>((uint8_t)type);
            write_cstring(key, klen);
        }

        template<typename T>
        void write_pair(bson_type type, const char* key, size_t klen, T value) {
            write_key(type, key, klen);
            m_buff->write(value);
        }

        template<typename T>
        T read_val(lua_State* L, slice* slice) {
            T* value = slice->read<T>();
            if (value == nullptr) {
                luaL_error(L, "decode can't unpack one value");
            }
            return *value;
        }

        void read_objectid(lua_State* L, slice* slice) {
            char buffer[32] = { 0 };
            static char hextxt[] = "0123456789abcdef";
            const char* text = read_bytes(L, slice, 12);
            for (size_t i = 0; i < 12; i++) {
                buffer[i * 2] = hextxt[(text[i] >> 4) & 0xf];
                buffer[i * 2 + 1] = hextxt[text[i] & 0xf];
            }
            lua_pushlstring(L, buffer, 24);
        }

        void write_objectid(lua_State* L, char* buffer, const char* hexoid) {
            for (int i = 0; i < 24; i += 2) {
                char hi, low;
                PHEX(hi, hexoid[i]);
                PHEX(low, hexoid[i + 1]);
                if (hi > 16 || low > 16) {
                    luaL_error(L, "Invalid hex text : %s", hexoid);
                }
                buffer[i / 2] = hi << 4 | low;
            }
        }

        void write_number(lua_State *L, const char* key, size_t klen) {
            if (lua_isinteger(L, -1)) {
                int64_t v = lua_tointeger(L, -1);
                if (v >= INT32_MIN && v <= INT32_MAX) {
                    write_pair<int32_t>(bson_type::BSON_INT32, key, klen, v);
                } else {
                    write_pair<int64_t>(bson_type::BSON_INT64, key, klen, v);
                }
            } else {
                write_pair<double>(bson_type::BSON_REAL, key, klen, lua_tonumber(L, -1));
            }
        }

        void pack_array(lua_State *L, int depth, size_t len) {
            // length占位
            char numkey[32];
            size_t offset = m_buff->size();
            m_buff->write<uint32_t>(0);
            for (size_t i = 1; i <= len; i++) {
                lua_rawgeti(L, -1, i);
                size_t len = bson_index(numkey, i - 1);
                pack_one(L, numkey, len, depth);
                lua_pop(L, 1);
            }
            m_buff->write<uint8_t>(0);
            uint32_t size = m_buff->size() - offset;
            m_buff->copy(offset, (uint8_t*)&size, sizeof(uint32_t));
        }

        void pack_order(lua_State* L, int depth, size_t len) {
            size_t sz;
            size_t offset = m_buff->size();
            m_buff->write<uint32_t>(0);
            for (int i = 1; i + 1 <= len; i += 2) {
                lua_rawgeti(L, -1, i);
                if (!lua_isstring(L, -1)) {
                    luaL_error(L, "Argument %d need a string", i);
                }
                const char* key = lua_tolstring(L, -1, &sz);
                lua_rawgeti(L, -2, i + 1);
                pack_one(L, key, sz, depth);
                lua_pop(L, 2);
            }
            m_buff->write<uint8_t>(0);
            uint32_t size = m_buff->size() - offset;
            m_buff->copy(offset, (uint8_t*)&size, sizeof(uint32_t));
        }

        bson_type check_doctype(lua_State *L, size_t raw_len) {
            if (raw_len == 0) return bson_type::BSON_DOCUMENT;
            lua_guard g(L);
            lua_pushnil(L);
            size_t cur_len = 0;
            while(lua_next(L, -2) != 0) {
                if (!lua_isinteger(L, -2)) {
                    return bson_type::BSON_DOCUMENT;
                }
                size_t key = lua_tointeger(L, -2);
                if (key <= 0 || key > raw_len) {
                    return bson_type::BSON_DOCUMENT;
                }
                cur_len++;
                lua_pop(L, 1);
            }
            return cur_len == raw_len ? bson_type::BSON_ARRAY : bson_type::BSON_DOCUMENT;
        }

        void pack_table(lua_State *L, const char* key, size_t len, int depth) {
            if (depth > max_bson_depth) {
                luaL_error(L, "Too depth while encoding bson");
            }
            int raw_len = lua_rawlen(L, -1);
            bson_type type = check_doctype(L, raw_len);
            write_key(type, key, len);
            if (type == bson_type::BSON_DOCUMENT) {
                lua_getfield(L, -1, "__order");
                auto no_order = lua_isnil(L, -1);
                lua_pop(L, 1);
                if (no_order) {
                    pack_dict(L, depth);
                } else {
                    lua_pushnil(L);
                    lua_setfield(L, -2, "__order");
                    pack_order(L, depth, raw_len);
                }
            } else {
                pack_array(L, depth, raw_len);
            }
        }

        void pack_bson_value(lua_State* L, bson_type type){
            switch(type) {
            case bson_type::BSON_MINKEY:
            case bson_type::BSON_MAXKEY:
            case bson_type::BSON_NULL:
                break;
            case bson_type::BSON_BINARY:
                pack_binary(L);
                break;
            case bson_type::BSON_DATE:
                pack_date(L);
                break;
            case bson_type::BSON_INT64:
            case bson_type::BSON_TIMESTAMP:
                pack_int64(L);
                break;
            case bson_type::BSON_ARRAY:
            case bson_type::BSON_JSCODE:
            case bson_type::BSON_DOCUMENT:
                pack_string(L);
                break;
            case bson_type::BSON_OBJECTID:
                pack_objectid(L);
                break;
            case bson_type::BSON_REGEX:
                pack_regex(L);
                break;
            default:
                luaL_error(L, "Invalid value type : %d", (int)type);
            }
        }

        void pack_one(lua_State *L, const char* key, size_t klen, int depth) {
            int vt = lua_type(L, -1);
            switch(vt) {
            case LUA_TNUMBER:
                write_number(L, key, klen);
                break;
            case LUA_TBOOLEAN:
                write_pair<bool>(bson_type::BSON_BOOLEAN, key, klen, lua_toboolean(L, -1));
                break;
            case LUA_TTABLE: {
                    lua_getfield(L, -1, "__type");
                    if (lua_type(L, -1) == LUA_TNUMBER) {
                        bson_type type = (bson_type)lua_tointeger(L, -1);
                        write_key(type, key, klen);
                        lua_pop(L, 1);
                        pack_bson_value(L, type);
                    } else {
                        lua_pop(L, 1);
                        pack_table(L, key, klen, depth + 1);
                    }
                }
                break;
            case LUA_TSTRING: {
                size_t sz;
                const char* buf = lua_tolstring(L, -1, &sz);
                if (sz > 2 && buf[0] == 0 && buf[1] != 0) {
                    write_key((bson_type)buf[1], key, klen);
                    m_buff->push_data((uint8_t*)(buf + 2), sz - 2);
                } else {
                    write_key(bson_type::BSON_STRING, key, klen);
                        write_string(buf, sz);
                    }
                }
                break;
            case LUA_TNIL:
                luaL_error(L, "Bson array has a hole (nil), Use bson.null instead");
                break;
            default:
                luaL_error(L, "Invalid value type : %s", lua_typename(L,vt));
            }
        }

        void pack_dict_data(lua_State *L, int depth, int kt) {
            if (kt == LUA_TSTRING) {
                size_t sz;
                const char* buf = lua_tolstring(L, -2, &sz);
                pack_one(L, buf, sz, depth);
                return;
            }
            if (lua_isinteger(L, -2)){
                char numkey[32];
                size_t len = bson_index(numkey, lua_tointeger(L, -2));
                pack_one(L, numkey, len, depth);
                return;
            }
            luaL_error(L, "Invalid key type : %s", lua_typename(L, kt));
        }

        void pack_dict(lua_State *L, int depth) {
            // length占位
            size_t offset = m_buff->size();
            m_buff->write<uint32_t>(0);
            lua_pushnil(L);
            while(lua_next(L, -2) != 0) {
                pack_dict_data(L, depth, lua_type(L, -2));
                lua_pop(L, 1);
            }
            m_buff->write<uint8_t>(0);
            uint32_t size = m_buff->size() - offset;
            m_buff->copy(offset, (uint8_t*)&size, sizeof(uint32_t));
        }

        const char* read_bytes(lua_State* L, slice* slice, size_t sz) {
            const char* dst = (const char*)slice->peek(sz);
            if (!dst) {
                throw lua_exception("invalid bson string , length = %lu", sz);
            }
            slice->erase(sz);
            return dst;
        }

        const char* read_string(lua_State* L, slice* slice, size_t& sz) {
            sz = (size_t)read_val<uint32_t>(L, slice);
            if (sz <= 0) {
                throw lua_exception("invalid bson string , length = %lu", sz);
            }
            sz = sz - 1;
            const char* dst = "";
            if (sz > 0) {
                dst = read_bytes(L, slice, sz);
            }
            slice->erase(1);
            return dst;
        }

        const char* read_cstring(slice* slice, size_t& l) {
            size_t sz;
            const char* dst = (const char*)slice->data(&sz);
            for (l = 0; l < sz; ++l) {
                if (dst[l] == '\0') {
                    slice->erase(l + 1);
                    return dst;
                }
                if (l == sz - 1) {
                    throw lua_exception("invalid bson block : cstring");
                }
            }
            throw lua_exception("invalid bson block : cstring");
            return "";
        }

        void unpack_key(lua_State* L, slice* slice, bool isarray) {
            size_t klen = 0;
            const char* key = read_cstring(slice, klen);
            if (isarray) {
                lua_pushinteger(L, std::stoll(key, nullptr, 10) + 1);
                return;
            }
            if (lua_stringtonumber(L, key) == 0) {
                lua_pushlstring(L, key, klen);
            }
        }

        void unpack_dict(lua_State* L, slice* slice, bool isarray) {
            uint32_t sz = read_val<uint32_t>(L, slice);
            if (slice->size() < sz - 4) {
                throw lua_exception("decode can't unpack one value");
            }
            lua_createtable(L, 0, 8);
            while (!slice->empty()) {
                size_t klen = 0;
                bson_type bt = (bson_type)read_val<uint8_t>(L, slice);
                if (bt == bson_type::BSON_EOO) break;
                unpack_key(L, slice, isarray);
                switch (bt) {
                case bson_type::BSON_REAL:
                    lua_pushnumber(L, read_val<double>(L, slice));
                    break;
                case bson_type::BSON_BOOLEAN:
                    lua_pushboolean(L, read_val<bool>(L, slice));
                    break;
                case bson_type::BSON_INT32:
                    lua_pushinteger(L, read_val<int32_t>(L, slice));
                    break;
                case bson_type::BSON_DATE:
                    lua_pushinteger(L, read_val<int64_t>(L, slice) / 1000);
                    break;
                case bson_type::BSON_INT64:
                case bson_type::BSON_TIMESTAMP:
                    lua_pushinteger(L, read_val<int64_t>(L, slice));
                    break;
                case bson_type::BSON_OBJECTID:
                    read_objectid(L, slice);
                    break;
                case bson_type::BSON_JSCODE:
                case bson_type::BSON_STRING:{
                        const char* s = read_string(L, slice, klen);
                        lua_pushlstring(L, s, klen);
                    }
                    break;
                case bson_type::BSON_BINARY: {
                        lua_createtable(L, 0, 4);
                        int32_t len = read_val<int32_t>(L, slice);
                        lua_pushinteger(L, (uint32_t)bt);
                        lua_setfield(L, -2, "__type");
                        lua_pushinteger(L, read_val<uint8_t>(L, slice));
                        lua_setfield(L, -2, "subtype");
                        const char* s = read_bytes(L, slice, len);
                        lua_pushlstring(L, s, len);
                        lua_setfield(L, -2, "binary");
                    }
                    break;
                case bson_type::BSON_REGEX: {
                        lua_createtable(L, 0, 4);
                        lua_pushinteger(L, (uint32_t)bt);
                        lua_setfield(L, -2, "__type");
                        lua_pushstring(L, read_cstring(slice, klen));
                        lua_setfield(L, -2, "pattern");
                        lua_pushstring(L, read_cstring(slice, klen));
                        lua_setfield(L, -2, "option");
                    }
                    break;
                case bson_type::BSON_DOCUMENT:
                    unpack_dict(L, slice, false);
                    break;
                case bson_type::BSON_ARRAY:
                    unpack_dict(L, slice, true);
                    break;
                case bson_type::BSON_MINKEY:
                case bson_type::BSON_MAXKEY:
                case bson_type::BSON_NULL: {
                        lua_createtable(L, 0, 2);
                        lua_pushinteger(L, (uint32_t)bt);
                        lua_setfield(L, -2, "__type");
                    }
                    break;
                default:
                    throw lua_exception("invalid bson type: %d", (int)bt);
                }
                lua_rawset(L, -3);
            }
        }
    private:
        luabuf* m_buff;
    };

    class mgocodec : public codec_base {
    public:
        virtual int load_packet(size_t data_len) {
            if (!m_slice) return 0;
            uint32_t* packet_len = (uint32_t*)m_slice->peek(sizeof(uint32_t));
            if (!packet_len) return 0;
            m_packet_len = *packet_len;
            if (m_packet_len > 0xffffff) return -1;
            if (m_packet_len > data_len) return 0;
            if (!m_slice->peek(m_packet_len)) return 0;
            return m_packet_len;
        }

        virtual uint8_t* encode(lua_State* L, int index, size_t* len) {
            m_buf->clean();
            m_buf->write<uint32_t>(0);
            m_buf->write<uint32_t>(lua_tointeger(L, 1));
            m_buf->write<uint32_t>(0);
            m_buf->write<uint32_t>(OP_MSG_CODE);
            m_buf->write<uint32_t>(0);
            m_buf->write<uint8_t>(0);
            lua_remove(L, 1);
            uint8_t* data = m_bson->encode_pairs(L, len);
            m_buf->copy(0, (uint8_t*)len, sizeof(uint32_t));
            return data;
        }

        virtual size_t decode(lua_State* L) {
            if (!m_slice) return 0;
            //skip length + request_id
            m_slice->erase(8);
            uint32_t session_id = m_bson->read_val<uint32_t>(L, m_slice);
            uint32_t opcode = m_bson->read_val<uint32_t>(L, m_slice);
            if (opcode != OP_MSG_CODE) {
                throw lua_exception("unsupported opcode: %d", opcode);
            }
            uint32_t flags = m_bson->read_val<uint32_t>(L, m_slice);
            if (flags > 0 && ((flags & OP_CHECKSUM) != 0 || ((flags ^ OP_MORE_COME) != 0))) {
                throw lua_exception("unsupported flags: %d", flags);
            }
            uint32_t payload = m_bson->read_val<uint8_t>(L, m_slice);
            if (payload != 0) {
                throw lua_exception("unsupported flags: %d", payload);
            }
            int otop = lua_gettop(L);
            lua_pushinteger(L, session_id);
            try {
                m_bson->unpack_dict(L, m_slice, false);
            } catch (const exception& e){
                lua_settop(L, otop);
                throw lua_exception(e.what());
            }
            return lua_gettop(L) - otop;
        }

        void set_bson(bson* bson) {
            m_bson = bson;
        }

    protected:
        bson* m_bson;
    };
}