#ifndef FF_PLAYER_H
#define FF_PLAYER_H

typedef struct VideoState VideoState;

int ffplayer_open(VideoState **state, char *input_filename);
int ffplayer_play_or_pause(VideoState *state);
int ffplayer_seek(VideoState *state, int value);
int ffplayer_fast_seek(VideoState *state, int forward);
int ffplayer_get_video_pkt(VideoState *state, AVPacket *pkt);
#if AUDIO_ENABLED
int ffplayer_get_audio_pkt(VideoState *state, AVPacket *pkt);
#endif
int ffplayer_close(VideoState *state);

#endif