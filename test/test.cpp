extern "C"
{
#include <ffplayer.h>
}

#include <iostream>
#include <future>

int main()
{
    std::string file = "/home/dragon/Videos/street_paris.mp4";
    std::string file1 = "/home/dragon/Videos/street_paris_1080p.mp4";

    FFContext *ctx1 = nullptr;
    FFContext *ctx2 = nullptr;
    auto a1 = std::async(std::launch::async, ffplayer_open, &ctx1, const_cast<char *>(file.c_str()));
    auto a2 = std::async(std::launch::async, ffplayer_open, &ctx2, const_cast<char *>(file1.c_str()));

    // TODO: ensure ffcontext is set

    int n = -1;
    while (n != 0)
    {
        std::cin >> n;
        switch (n)
        {
        case 5:
            ffplayer_play_or_pause(ctx1);
            break;
        case 3:
            ffplayer_play_or_pause(ctx2);
            break;

        case 4:
            ffplayer_fast_seek(ctx1, 0);
            break;

        case 6:
            ffplayer_fast_seek(ctx1, 1);
            break;

        case 0:
            ffplayer_close(ctx1);
            ffplayer_close(ctx2);
            ctx1 = nullptr;
            ctx2 = nullptr;
            break;

        default:
            break;
        }
    }

    a1.wait();
    a2.wait();
}