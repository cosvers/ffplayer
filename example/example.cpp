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
    ffplayer_open(&state1, const_cast<char *>(file.c_str()));

    auto a1 = std::async(std::launch::async,
                         [&]()
                         {
                             while (1)
                             {
                                 AVPacket *pkt1 = nullptr;

                                 pkt1 = av_packet_alloc();
                                 if (!pkt1)
                                 {
                                     std::cout << "Could not allocate packet 1.\n";
                                 }

                                 if (-1 == ffplayer_get_video_pkt(state1, pkt1))
                                 {
                                     break;
                                 }

                                 std::cout << " packet1 size: " << pkt1->size << " pts: " << pkt1->pts << "\n";

                                 av_packet_free(&pkt1);
                             }
                         });

    int n, ret = 0;
    while (!ret)
    {
        std::cin >> n;
        switch (n)
        {
        case 5:
            ret = ffplayer_play_or_pause(state1);
            break;

        case 3:
            ret = ffplayer_play_or_pause(state1);
            break;

        case 4:
            ret = ffplayer_fast_seek(state1, 0);
            break;

        case 6:
            ret = ffplayer_fast_seek(state1, 1);
            break;

        case 0:
            ffplayer_close(state1);
            state1 = nullptr;
            ret = -1;
            break;

        default:
            break;
        }
    }

    a1.wait();
}