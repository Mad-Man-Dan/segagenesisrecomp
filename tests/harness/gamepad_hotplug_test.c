#include "gamepad.h"
#include "input_map.h"

#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>

static void require_true(int condition, const char *message)
{
    if (condition) return;
    fprintf(stderr, "gamepad_hotplug_test: %s (SDL: %s)\n", message, SDL_GetError());
    exit(1);
}

static void drain_controller_events(void)
{
    SDL_Event event;
    SDL_PumpEvents();
    while (SDL_PollEvent(&event))
        gamepad_handle_event(&event);
    SDL_GameControllerUpdate();
}

static int attach_virtual_controller(void)
{
    int index = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER,
                                          SDL_CONTROLLER_AXIS_MAX,
                                          SDL_CONTROLLER_BUTTON_MAX, 0);
    require_true(index >= 0, "could not attach virtual controller");

    SDL_JoystickGUID guid = SDL_JoystickGetDeviceGUID(index);
    char guid_text[33];
    SDL_JoystickGetGUIDString(guid, guid_text, sizeof(guid_text));
    char mapping[512];
    snprintf(mapping, sizeof(mapping),
             "%s,GenesisRecomp Virtual,a:b0,b:b1,x:b2,y:b3,back:b4,guide:b5,"
             "start:b6,leftstick:b7,rightstick:b8,leftshoulder:b9,rightshoulder:b10,"
             "dpup:b11,dpdown:b12,dpleft:b13,dpright:b14,"
             "leftx:a0,lefty:a1,rightx:a2,righty:a3,lefttrigger:a4,righttrigger:a5,",
             guid_text);
    require_true(SDL_GameControllerAddMapping(mapping) >= 0,
                 "could not register virtual controller mapping");
    require_true(SDL_IsGameController(index),
                 "virtual joystick was not recognized as a game controller");
    return index;
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    require_true(SDL_Init(SDL_INIT_GAMECONTROLLER) == 0, "SDL initialization failed");
    input_map_init_defaults();
    gamepad_init();
    int baseline[2] = {
        gamepad_player_connected(0), gamepad_player_connected(1)
    };
    int slot = !baseline[0] ? 0 : (!baseline[1] ? 1 : -1);
    if (slot < 0) {
        gamepad_shutdown();
        SDL_Quit();
        puts("gamepad_hotplug_test: SKIP (both player slots already occupied)");
        return 0;
    }

    int first = attach_virtual_controller();
    drain_controller_events();
    require_true(gamepad_player_connected(slot),
                 "hot-added controller was not assigned to the free player slot");

    require_true(SDL_JoystickDetachVirtual(first) == 0,
                 "could not detach virtual controller");
    drain_controller_events();
    require_true(!gamepad_player_connected(slot), "removed controller remained assigned");
    require_true(gamepad_player_connected(0) == baseline[0] &&
                 gamepad_player_connected(1) == baseline[1],
                 "removal disturbed a pre-existing controller assignment");

    int second = attach_virtual_controller();
    drain_controller_events();
    require_true(gamepad_player_connected(slot),
                 "re-added controller was not reassigned to the free player slot");
    require_true(SDL_JoystickDetachVirtual(second) == 0,
                 "could not detach re-added virtual controller");
    drain_controller_events();

    gamepad_shutdown();
    SDL_Quit();
    puts("gamepad_hotplug_test: PASS");
    return 0;
}
