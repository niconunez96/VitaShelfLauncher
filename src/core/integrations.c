#include "integrations.h"
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SGDB_API "https://www.steamgriddb.com/api/v2"
/* Owned by the single network worker, like the existing RomM response buffer. */
static char response[64*1024];
typedef struct { const char *begin, *end; } Json;

static const char *space(const char *p) { while (*p && isspace((unsigned char)*p)) p++; return p; }
static int hex(char c) {
    if (c>='0' && c<='9') return c-'0';
    if (c>='a' && c<='f') return c-'a'+10;
    if (c>='A' && c<='F') return c-'A'+10;
    return -1;
}
static bool unicode(const char **cursor, unsigned int *value) {
    unsigned int n=0, i;
    for (i=0;i<4;i++) {
        int digit=hex(**cursor);
        if (digit<0) return false;
        n=(n<<4)|(unsigned)digit; (*cursor)++;
    }
    *value=n; return true;
}
/* Decode strings rather than matching raw JSON: titles commonly contain Unicode. */
static const char *string(const char *p, char *out, size_t capacity) {
    size_t used=0;
    if (*p++!='"') return NULL;
    while (*p && *p!='"') {
        unsigned int code=(unsigned char)*p++;
        unsigned char encoded[4]; size_t count=1;
        if (code<32) return NULL;
        if (code=='\\') {
            char escaped=*p++;
            if (!escaped) return NULL;
            switch (escaped) {
                case '"': case '\\': case '/': code=(unsigned char)escaped; break;
                case 'b': code='\b'; break; case 'f': code='\f'; break;
                case 'n': code='\n'; break; case 'r': code='\r'; break; case 't': code='\t'; break;
                case 'u':
                    if (!unicode(&p,&code) || !code) return NULL;
                    if (code>=0xd800 && code<=0xdbff) {
                        unsigned int low;
                        if (*p++!='\\' || *p++!='u' || !unicode(&p,&low) || low<0xdc00 || low>0xdfff) return NULL;
                        code=0x10000+((code-0xd800)<<10)+(low-0xdc00);
                    } else if (code>=0xdc00 && code<=0xdfff) return NULL;
                    if (code>=0x10000) { encoded[0]=0xf0|(code>>18); count=4; }
                    else if (code>=0x800) { encoded[0]=0xe0|(code>>12); count=3; }
                    else if (code>=0x80) { encoded[0]=0xc0|(code>>6); count=2; }
                    break;
                default: return NULL;
            }
        }
        if (count==1) encoded[0]=(unsigned char)code;
        else for (size_t i=1;i<count;i++) encoded[i]=0x80|((code>>(6*(count-1-i)))&63);
        if (out) {
            if (used+count>=capacity) return NULL;
            memcpy(out+used,encoded,count);
        }
        used+=count;
    }
    if (*p!='"') return NULL;
    if (out) out[used]=0;
    return p+1;
}
static const char *skip(const char *p, int depth) {
    char closing;
    if (depth>16) return NULL;
    p=space(p);
    if (*p=='"') return string(p,NULL,0);
    if (*p=='{' || *p=='[') {
        closing=*p++=='{'?'}':']'; p=space(p);
        if (*p==closing) return p+1;
        for (;;) {
            if (closing=='}') {
                p=string(p,NULL,0); if (!p || *(p=space(p))!=':') return NULL;
                p++;
            }
            p=skip(p,depth+1); if (!p) return NULL;
            p=space(p); if (*p==closing) return p+1;
            if (*p++!=',') return NULL;
            p=space(p);
        }
    }
    if (!strncmp(p,"true",4) || !strncmp(p,"null",4)) return p+4;
    if (!strncmp(p,"false",5)) return p+5;
    if (*p=='-' || isdigit((unsigned char)*p)) {
        char *end;
        (void)strtod(p,&end);
        return end>p?end:NULL;
    }
    return NULL;
}
static bool member(Json object, const char *key, Json *value) {
    const char *p=space(object.begin);
    if (*p++!='{') return false;
    p=space(p);
    while (p<object.end && *p!='}') {
        char name[64]; const char *end;
        p=string(p,name,sizeof(name)); if (!p || *(p=space(p))!=':') return false;
        p=space(p+1); end=skip(p,0); if (!end || end>object.end) return false;
        if (!strcmp(name,key)) { value->begin=p; value->end=end; return true; }
        p=space(end); if (*p!=',') break;
        p=space(p+1);
    }
    return false;
}
static bool text(Json object, const char *key, char *out, size_t capacity) {
    Json value;
    return member(object,key,&value) && string(value.begin,out,capacity)==value.end;
}
static bool number(Json object, const char *key, unsigned long *out) {
    Json value; char *end; unsigned long n;
    if (!member(object,key,&value) || !isdigit((unsigned char)*value.begin)) return false;
    n=strtoul(value.begin,&end,10);
    if (end!=value.end || !n || n>0x7fffffffUL) return false;
    *out=n; return true;
}
static bool payload(Json *data) {
    Json root={response,response+strlen(response)}, success;
    const char *end=skip(root.begin,0);
    return end && !*space(end) && member(root,"success",&success) &&
        success.end-success.begin==4 && !memcmp(success.begin,"true",4) &&
        member(root,"data",data) && *data->begin=='[';
}
static VpConnectionState request(const char *url, const char *key, VpRommProbeBackend backend) {
    long status;
    if (!backend.execute) return VP_CONNECTION_UNREACHABLE;
    response[0]=0;
    status=backend.execute(url,key,response,sizeof(response),backend.context);
    if (status==401 || status==403) return VP_CONNECTION_INVALID_KEY;
    if (status==429) return VP_CONNECTION_RATE_LIMITED;
    if (status<200 || status>=300) return VP_CONNECTION_UNREACHABLE;
    return VP_CONNECTION_CONNECTED;
}

bool vp_image_service_endpoint(const char *host, const char *endpoint, char *url, size_t capacity) {
    char base[VP_URL]; size_t length; const char *authority;
    int written;
    if (!host || !*host || !endpoint || !url || strpbrk(host,"?#@\r\n\t ")) return false;
    written=snprintf(base,sizeof(base),"%s%s",strstr(host,"://")?"":"http://",host);
    if (written<0 || written>=(int)sizeof(base)) return false;
    if (!strncmp(base,"https://",8)) authority=base+8;
    else if (!strncmp(base,"http://",7)) authority=base+7;
    else return false;
    if (!*authority || *authority=='/') return false;
    length=strlen(base);
    while (length && base[length-1]=='/') base[--length]=0;
    if (length>=6 && !strcmp(base+length-6,"/image")) base[length-=6]=0;
    if (base+length<=authority) return false;
    written=snprintf(url,capacity,"%s/%s",base,endpoint);
    return written>0 && (size_t)written<capacity;
}
VpConnectionState vp_image_service_check(const char *host, VpRommProbeBackend backend) {
    char url[VP_URL]; VpConnectionState state;
    if (!host || !*host) return VP_CONNECTION_UNCONFIGURED;
    if (!vp_image_service_endpoint(host,"health",url,sizeof(url))) return VP_CONNECTION_INVALID_RESPONSE;
    state=request(url,NULL,backend);
    if (state!=VP_CONNECTION_CONNECTED) return state;
    return !strcmp(space(response),"ok\n") || !strcmp(space(response),"ok")?
        VP_CONNECTION_CONNECTED:VP_CONNECTION_INVALID_RESPONSE;
}
VpConnectionState vp_steamgrid_check(const char *key, VpRommProbeBackend backend) {
    Json data; VpConnectionState state;
    if (!key || !*key) return VP_CONNECTION_UNCONFIGURED;
    state=request(SGDB_API "/search/autocomplete/Persona",key,backend);
    if (state!=VP_CONNECTION_CONNECTED) return state;
    return payload(&data)?state:VP_CONNECTION_INVALID_RESPONSE;
}

static void normalized(const char *source, char *out, size_t capacity) {
    size_t i=0;
    while (*source && i+1<capacity) {
        unsigned char c=(unsigned char)*source++;
        /* Ignore trademark symbols and presentation punctuation, not subtitles. */
        if ((c==0xc2 && (unsigned char)source[0]==0xae) ||
            (c==0xe2 && (unsigned char)source[0]==0x84 && (unsigned char)source[1]==0xa2)) {
            source+=c==0xc2?1:2; continue;
        }
        if (c>=128 || isalnum(c)) out[i++]=c>=128?(char)c:(char)tolower(c);
    }
    out[i]=0;
}
static bool encode_term(const char *term, char *out, size_t capacity) {
    static const char digits[]="0123456789ABCDEF";
    size_t i=0;
    while (*term) {
        unsigned char c=(unsigned char)*term++;
        bool safe=isalnum(c) && c<128;
        if (i+(safe?1:3)>=capacity) return false;
        if (safe) out[i++]=(char)c;
        else { out[i++]='%'; out[i++]=digits[c>>4]; out[i++]=digits[c&15]; }
    }
    out[i]=0; return true;
}
VpArtworkResult vp_steamgrid_cover(VpItem *item, const char *key, VpRommProbeBackend backend,
                                   VpConnectionState *state) {
    char url[VP_URL*3], term[VP_TEXT*3], wanted[VP_TEXT];
    unsigned long game=0;
    Json data; const char *p;
    if (!item || !state || !key || !*key || item->source!=VP_SOURCE_LOCAL) return VP_ARTWORK_FAILED;
    if (item->remote_id[0] && strspn(item->remote_id,"0123456789")==strlen(item->remote_id))
        game=strtoul(item->remote_id,NULL,10);
    if (!game) {
        if (!encode_term(item->title,term,sizeof(term))) return VP_ARTWORK_FAILED;
        (void)snprintf(url,sizeof(url),SGDB_API "/search/autocomplete/%s",term);
        *state=request(url,key,backend);
        if (*state!=VP_CONNECTION_CONNECTED) return VP_ARTWORK_FAILED;
        if (!payload(&data)) { *state=VP_CONNECTION_INVALID_RESPONSE; return VP_ARTWORK_FAILED; }
        normalized(item->title,wanted,sizeof(wanted));
        p=space(data.begin+1);
        while (*p && *p!=']') {
            const char *end=skip(p,0); Json object={p,end};
            char name[VP_TEXT], match[VP_TEXT]; unsigned long id;
            if (!end) return VP_ARTWORK_FAILED;
            if (text(object,"name",name,sizeof(name)) && number(object,"id",&id)) {
                normalized(name,match,sizeof(match));
                if (wanted[0] && !strcmp(wanted,match)) {
                    if (game && game!=id) return VP_ARTWORK_NOT_FOUND;
                    game=id;
                }
            }
            p=space(end); if (*p!=',') break;
            p=space(p+1);
        }
        if (!game) return VP_ARTWORK_NOT_FOUND;
    }
    (void)snprintf(url,sizeof(url),SGDB_API "/grids/game/%lu?dimensions=600x900,660x930,342x482&types=static&mimes=image/png,image/jpeg&limit=10",game);
    *state=request(url,key,backend);
    if (*state!=VP_CONNECTION_CONNECTED) return VP_ARTWORK_FAILED;
    if (!payload(&data)) { *state=VP_CONNECTION_INVALID_RESPONSE; return VP_ARTWORK_FAILED; }
    p=space(data.begin+1);
    while (*p && *p!=']') {
        const char *end=skip(p,0); Json object={p,end};
        char cover[VP_URL], mime[32];
        if (!end) return VP_ARTWORK_FAILED;
        if (text(object,"url",cover,sizeof(cover)) && !strncmp(cover,"https://",8) &&
            text(object,"mime",mime,sizeof(mime)) && (!strcmp(mime,"image/png") || !strcmp(mime,"image/jpeg"))) {
            (void)snprintf(item->remote_id,sizeof(item->remote_id),"%lu",game);
            (void)snprintf(item->cover_url,sizeof(item->cover_url),"%s",cover);
            return VP_ARTWORK_FOUND;
        }
        p=space(end); if (*p!=',') break;
        p=space(p+1);
    }
    return VP_ARTWORK_NOT_FOUND;
}
