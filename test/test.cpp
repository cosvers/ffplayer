extern "C"
{
#include "ffplay.h"
}

#include <iostream>
#include <future>

int main()
{
    std::string file = "/home/dragon/Videos/street_paris.mp4";
    auto a1 = std::async(std::launch::async, ffplayer_open, const_cast<char *>(file.c_str()));

    int n = -1;
    while (n != 0)
    {
        std::cin >> n;

        switch (n)
        {
        case 5:
            ffplayer_play_or_pause();
            break;

        case 4:
            ffplayer_fast_seek(0);
            break;

        case 6:
            ffplayer_fast_seek(1);
            break;

        case 0:
            ffplayer_close();
            break;

        default:
            break;
        }
    }

    a1.wait();
}