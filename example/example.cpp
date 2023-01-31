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
    std::string live = "rtsp://localhost:8554/stream";

    VideoState *state1;
    ffplayer_open(&state1, const_cast<char *>(live.c_str()));

    auto a1 = std::async(std::launch::async,
                         [&]()
                         {
                             while (1)
                             {
                                 AVPacket *pkt = nullptr;

                                 pkt = av_packet_alloc();
                                 if (!pkt)
                                 {
                                     std::cout << "Could not allocate packet 1.\n";
                                 }

                                 if (-1 == ffplayer_get_video_pkt(state1, pkt))
                                 {
                                     break;
                                 }

                                 if (pkt->size == 0)
                                 {
                                     std::this_thread::sleep_for(std::chrono::milliseconds(10));
                                 }

                                 std::cout << "Packet size: " << pkt->size << " PTS: " << pkt->pts << "\n";

                                 av_packet_free(&pkt);
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