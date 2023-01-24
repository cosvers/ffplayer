int ffplayer_open(char *input_filename);
int ffplayer_play_or_pause();
int ffplayer_seek(int value, int forward, int fast);
int ffplayer_next_frame(void *frame);
int ffplayer_close();