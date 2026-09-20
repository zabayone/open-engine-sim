#include "desktop_platform_sdl.h"
#include <SDL3/SDL.h>
#include <gtest/gtest.h>

TEST(SdlPlatformInput, PreservesClickLocationWhenPointerMovesLaterInTheFrame) {
    ASSERT_TRUE(SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy"));
    ASSERT_TRUE(SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy"));
    DesktopPlatformSdl platform;
    ASSERT_TRUE(platform.initialize("Input test", 640, 480));
    platform.pumpEvents();
    SDL_Event event{};
    event.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    event.button.button = SDL_BUTTON_LEFT;
    event.button.x = 120;
    event.button.y = 80;
    ASSERT_TRUE(SDL_PushEvent(&event));
    event.type = SDL_EVENT_MOUSE_BUTTON_UP;
    ASSERT_TRUE(SDL_PushEvent(&event));
    event.type = SDL_EVENT_MOUSE_MOTION;
    event.motion.x = 300;
    event.motion.y = 200;
    ASSERT_TRUE(SDL_PushEvent(&event));
    platform.pumpEvents();
    int x, y;
    platform.mousePressPosition(&x, &y);
    EXPECT_EQ(x, 120);
    EXPECT_EQ(y, platform.windowHeight() - 80);
    platform.mouseReleasePosition(&x, &y);
    EXPECT_EQ(x, 120);
    EXPECT_EQ(y, platform.windowHeight() - 80);
    platform.mousePosition(&x, &y);
    EXPECT_EQ(x, 300);
    EXPECT_TRUE(platform.wasMouseButtonPressed(DesktopMouseButton::Left));
    EXPECT_TRUE(platform.wasMouseButtonReleased(DesktopMouseButton::Left));
    platform.shutdown();
}
