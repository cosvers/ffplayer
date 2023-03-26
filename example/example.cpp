#include <iostream>
#include <future>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <ffplayer.h>
}

int main()
{
    std::string file = "/home/dragon/Videos/worker-zone-detection.mp4";
    std::string file1 = "/home/dragon/Videos/street_paris_1080p.mp4";
    std::string live = "rtsp://localhost:8554/stream";

    VideoState *state;
    ffplayer_open(&state, const_cast<char *>(file1.c_str()));

    int duration = ffplayer_get_duration(state);
    int fps = ffplayer_get_fps(state);
    if (duration > 0)
    {
        int hours, mins, secs;
        hours = duration / 3600;
        mins = (duration % 3600) / 60;
        secs = (duration % 60);

        std::cout << "Duration: " << hours << ":" << mins << ":" << secs << "\n";
    }

    if (fps > 0)
        std::cout << "FPS: " << fps << "\n";

    std::cout << duration << " - " << fps << "\n";

    auto a1 = std::async(std::launch::async,
                         [&]()
                         {
                             while (1)
                             {
                                 AVPacket *pkt = nullptr;
                                 double curr_pos = 0;

                                 pkt = av_packet_alloc();
                                 if (!pkt)
                                 {
                                     std::cout << "Could not allocate packet.\n";
                                 }

                                 if (-1 == ffplayer_get_video_pkt(state, pkt, &curr_pos))
                                 {
                                     break;
                                 }

                                 std::cout << "Packet size: " << pkt->size << " PTS: " << pkt->pts << " Pos: " << curr_pos << "\n";

                                 av_packet_free(&pkt);
                             }
                         });

    int n, ret = 0, random = 0;
    while (!ret)
    {
        std::cin >> n;
        switch (n)
        {
        case 5:
            ret = ffplayer_play_or_pause(state);
            break;

        case 3:
            random = rand() % duration;
            ret = ffplayer_seek(state, random);
            break;

        case 4:
            ret = ffplayer_fast_seek(state, 0);
            break;

        case 6:
            ret = ffplayer_fast_seek(state, 1);
            break;

        case 0:
            ffplayer_close(state);
            state = nullptr;
            ret = -1;
            break;

        default:
            break;
        }
    }

    a1.wait();
}