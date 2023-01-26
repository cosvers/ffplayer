#ifndef FF_PLAYER_H
#define FF_PLAYER_H

typedef struct FFContext FFContext;

void ffplayer_open(FFContext **ctxt, char *input_filename);
int ffplayer_play_or_pause(FFContext *ctxt);
int ffplayer_seek(FFContext *ctxt, int value);
int ffplayer_fast_seek(FFContext *ctxt, int forward);
int ffplayer_get_video_pkt(FFContext *ctxt, void *pkt);
int ffplayer_get_audio_pkt(FFContext *ctxt, void *pkt);
int ffplayer_close(FFContext *ctxt);

#endif