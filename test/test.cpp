#include <iostream>
#include <future>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <ffplayer.h>
}

int main()
{
    std::string file = "/home/dragon/Videos/one-by-one-person-detection.mp4";
    std::string file1 = "/home/dragon/Videos/street_paris_1080p.mp4";

    VideoState *state1;
    ffplayer_open(&state1, const_cast<char *>(file1.c_str()));

    int i = 0;
    while (true)
    {
        AVPacket *pkt1 = nullptr;
        pkt1 = av_packet_alloc();
        if (!pkt1)
        {
            std::cout << "Could not allocate packet 1.\n";
        }

        ffplayer_get_video_pkt(state1, pkt1);

        i++;
        std::cout << " packet1 size: " << pkt1->size << " pts: " << pkt1->pts << " i:" << i << "\n";

        if (i > 100000)
        {
            printf("Program finished\n");
            break;
        }

        av_packet_free(&pkt1);
    }

    ffplayer_close(state1);

    /*
        int n = -1;
        while (n != 0)
        {
            std::cin >> n;
            switch (n)
            {
            case 5:
                ffplayer_play_or_pause(state1);
                break;

            case 3:
                ffplayer_play_or_pause(state1);
                break;

            case 4:
                ffplayer_fast_seek(state1, 0);
                break;

            case 6:
                ffplayer_fast_seek(state1, 1);
                break;

            case 0:
                ffplayer_close(state1);
                ffplayer_close(state2);
                state1 = nullptr;
                state2 = nullptr;
                break;

            default:
                break;
            }
        }*/
}