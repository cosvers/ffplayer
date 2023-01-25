int ffplayer_open(char *input_filename);
int ffplayer_play_or_pause();
int ffplayer_seek(int value);
int ffplayer_fast_seek(int forward);
int ffplayer_next_frame(void *frame);
int ffplayer_close();