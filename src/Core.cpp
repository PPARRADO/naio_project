#include <iostream>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <chrono>
#include <unistd.h>
#include <ApiCodec/ApiMotorsPacket.hpp>
#include <HaGyroPacket.hpp>
#include <HaAcceleroPacket.hpp>
#include <ApiCommandPacket.hpp>
#include <zlib.h>
#include <ApiWatchdogPacket.hpp>
#include "Core.hpp"

using namespace std;
using namespace std::chrono;

#define DEBUG_INTERFACE 1

// #################################################
//
Core::Core() :
        stopThreadAsked_{false},
        threadStarted_{false},
        graphicThread_{},
        hostAdress_{"10.0.1.1"},
        hostPort_{5555},
        socketConnected_{false},
        naioCodec_{},
        sendPacketList_{},
        ha_lidar_packet_ptr_{nullptr},
        ha_odo_packet_ptr_{nullptr},
        api_post_packet_ptr_{nullptr},
        ha_gps_packet_ptr_{nullptr},
        controlType_{ControlType::CONTROL_TYPE_MANUAL},
        last_motor_time_{0L},
        imageNaioCodec_{},
        last_left_motor_{0},
        last_right_motor_{0},
        last_image_received_time_{0} {
    uint8_t fake = 0;

    for (int i = 0; i < 1000000; i++) {
        if (fake >= 255) {
            fake = 0;
        }

        last_images_buffer_[i] = fake;

        fake++;
    }
    buttons = new SDL_Rect[8];
}

// #################################################
//
Core::~Core() {
    delete[] buttons;
}

// #################################################
//
void
Core::init(std::string hostAdress, uint16_t hostPort) {
    hostAdress_ = hostAdress;
    hostPort_ = hostPort;

    stopThreadAsked_ = false;
    threadStarted_ = false;
    socketConnected_ = false;

    imageServerThreadStarted_ = false;
    stopImageServerThreadAsked_ = false;

    serverReadthreadStarted_ = false;
    stopServerWriteThreadAsked_ = false;

    posX = 0.0;
    posY = 0.0;
    distRoueGauche = 0.0;
    distRoueDroite = 0.0;
    teta = 0.0;
    fakeTime = 0;

    // ignore unused screen
    (void) screen_;

    for (int i = 0; i < SDL_NUM_SCANCODES; i++) {
        sdlKey_[i] = 0;
    }

    mouse_pos_x = -1;
    mouse_pos_y = -1;
    command_interface = false;

    std::cout << "Connecting to : " << hostAdress << ":" << hostPort << std::endl;

    struct sockaddr_in server;

    //Create socket
#if DEBUG_INTERFACE == 1
    socket_desc_ = socket(AF_INET, SOCK_STREAM, 0);

    if (socket_desc_ == -1) {
        std::cout << "Could not create socket" << std::endl;
    }

    server.sin_addr.s_addr = inet_addr(hostAdress.c_str());
    server.sin_family = AF_INET;
    server.sin_port = htons(hostPort);

    //Connect to remote server
    if (connect(socket_desc_, (struct sockaddr *) &server, sizeof(server)) < 0) {
        puts("connect error");
    }
    else {
        puts("Connected\n");
        socketConnected_ = true;
    }
#endif

    // creates main thread
    graphicThread_ = std::thread(&Core::graphic_thread, this);

    info_thread = std::thread(&Core::calc_info, this);

#if DEBUG_INTERFACE == 1
    serverReadThread_ = std::thread(&Core::server_read_thread, this);

    serverWriteThread_ = std::thread(&Core::server_write_thread, this);

    imageServerThread_ = std::thread(&Core::image_server_thread, this);

#endif
}

// #################################################
//
void
Core::stop() {
    if (threadStarted_) {
        stopThreadAsked_ = true;

        graphicThread_.join();

        threadStarted_ = false;
    }
}

// #################################################
//
void Core::stopServerReadThread() {
    if (serverReadthreadStarted_) {
        stopServerReadThreadAsked_ = true;

        serverReadThread_.join();

        serverReadthreadStarted_ = false;
    }
}

// #################################################
// thread function
void Core::server_read_thread() {
    std::cout << "Starting server read thread !" << std::endl;

    uint8_t receiveBuffer[4000000];

    while (!stopServerReadThreadAsked_) {
        // any time : read incoming messages.
        int readSize = (int) read(socket_desc_, receiveBuffer, 4000000);

        if (readSize > 0) {
            bool packetHeaderDetected = false;

            bool atLeastOnePacketReceived = naioCodec_.decode(receiveBuffer, static_cast<uint>( readSize ),
                                                              packetHeaderDetected);

            // manage received messages
            if (atLeastOnePacketReceived) {
                for (auto &&packetPtr : naioCodec_.currentBasePacketList) {
                    manageReceivedPacket(packetPtr);
                }

                naioCodec_.currentBasePacketList.clear();
            }
        }
    }

    serverReadthreadStarted_ = false;
    stopServerReadThreadAsked_ = false;
}

// #################################################
//
void
Core::graphic_thread() {
    std::cout << "Starting main thread." << std::endl;

    // create graphics
    screen_ = initSDL("Api Client", 1300, 730);

    // prepare timers for real time operations
    milliseconds ms = duration_cast<milliseconds>(system_clock::now().time_since_epoch());

    int64_t now = static_cast<int64_t>( ms.count());
    int64_t duration = MAIN_GRAPHIC_DISPLAY_RATE_MS;
    int64_t nextTick = now + duration;

    threadStarted_ = true;

    while (!stopThreadAsked_) {
        ms = duration_cast<milliseconds>(system_clock::now().time_since_epoch());
        now = static_cast<int64_t>( ms.count());

        // Test keyboard input.
        // send commands related to keyboard.
        if (now >= nextTick) {
            nextTick = now + duration;

            if (asked_start_video_) {
                ApiCommandPacketPtr api_command_packet_zlib_off = std::make_shared<ApiCommandPacket>(
                        ApiCommandPacket::CommandType::TURN_OFF_IMAGE_ZLIB_COMPRESSION);
                ApiCommandPacketPtr api_command_packet_stereo_on = std::make_shared<ApiCommandPacket>(
                        ApiCommandPacket::CommandType::TURN_ON_API_RAW_STEREO_CAMERA_PACKET);

                sendPacketListAccess_.lock();
                sendPacketList_.emplace_back(api_command_packet_zlib_off);
                sendPacketList_.emplace_back(api_command_packet_stereo_on);
                sendPacketListAccess_.unlock();

                asked_start_video_ = false;
            }

            if (asked_stop_video_) {
                ApiCommandPacketPtr api_command_packet_stereo_off = std::make_shared<ApiCommandPacket>(
                        ApiCommandPacket::CommandType::TURN_OFF_API_RAW_STEREO_CAMERA_PACKET);

                sendPacketListAccess_.lock();
                sendPacketList_.emplace_back(api_command_packet_stereo_off);
                sendPacketListAccess_.unlock();

                asked_stop_video_ = false;
            }
        }

        readSDLKeyboard();
        manageSDLKeyboard();

        // drawing part.
        SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255); // the rect color (solid red)
        SDL_Rect background;
        background.w = 1200;
        background.h = 483;
        background.y = 0;
        background.x = 0;

        SDL_RenderFillRect(renderer_, &background);

        draw_robot();

        uint16_t lidar_distance_[271];

        ha_lidar_packet_ptr_access_.lock();

        if (ha_lidar_packet_ptr_ != nullptr) {
            for (int i = 0; i < 271; i++) {
                lidar_distance_[i] = ha_lidar_packet_ptr_->distance[i];
            }
        } else {
            for (int i = 0; i < 271; i++) {
                lidar_distance_[i] = 5000;
            }
        }

        ha_lidar_packet_ptr_access_.unlock();

        draw_lidar(lidar_distance_);

        draw_images();

        draw_command_interface(810, 10);

        // ##############################################
        char gyro_buff[100];

        ha_gyro_packet_ptr_access_.lock();
        HaGyroPacketPtr ha_gyro_packet_ptr = ha_gyro_packet_ptr_;
        ha_gyro_packet_ptr_access_.unlock();

        if (ha_gyro_packet_ptr != nullptr) {
            snprintf(gyro_buff, sizeof(gyro_buff), "Gyro  : %d ; %d, %d", ha_gyro_packet_ptr->x, ha_gyro_packet_ptr->y,
                     ha_gyro_packet_ptr->z);

            //std::cout << gyro_buff << std::endl;
        } else {
            snprintf(gyro_buff, sizeof(gyro_buff), "Gyro  : N/A ; N/A, N/A");
        }

        ha_accel_packet_ptr_access_.lock();
        HaAcceleroPacketPtr ha_accel_packet_ptr = ha_accel_packet_ptr_;
        ha_accel_packet_ptr_access_.unlock();

        char accel_buff[100];
        if (ha_accel_packet_ptr != nullptr) {
            snprintf(accel_buff, sizeof(accel_buff), "Accel : %d ; %d, %d", ha_accel_packet_ptr->x,
                     ha_accel_packet_ptr->y, ha_accel_packet_ptr->z);

            //std::cout << accel_buff << std::endl;
        } else {
            snprintf(accel_buff, sizeof(accel_buff), "Accel : N/A ; N/A, N/A");
        }

        ha_odo_packet_ptr_access.lock();
        HaOdoPacketPtr ha_odo_packet_ptr = ha_odo_packet_ptr_;
        ha_odo_packet_ptr_access.unlock();

        char odo_buff[100];
        if (ha_odo_packet_ptr != nullptr) {
            snprintf(odo_buff, sizeof(odo_buff), "ODO -> RF : %d ; RR : %d ; RL : %d, FL : %d", ha_odo_packet_ptr->fr,
                     ha_odo_packet_ptr->rr, ha_odo_packet_ptr->rl, ha_odo_packet_ptr->fl);

            //std::cout << odo_buff << std::endl;

        } else {
            snprintf(odo_buff, sizeof(odo_buff), "ODO -> RF : N/A ; RR : N/A ; RL : N/A, FL : N/A");
        }

        ha_gps_packet_ptr_access_.lock();
        HaGpsPacketPtr ha_gps_packet_ptr = ha_gps_packet_ptr_;
        ha_gps_packet_ptr_access_.unlock();

        char gps1_buff[100];
        char gps2_buff[100];
        char info[150];
        char info2[150];
        if (ha_gps_packet_ptr_ != nullptr) {
            snprintf(gps1_buff, sizeof(gps1_buff), "GPS -> lat : %lf ; lon : %lf ; alt : %lf", ha_gps_packet_ptr->lat,
                     ha_gps_packet_ptr->lon, ha_gps_packet_ptr->alt);
            snprintf(gps2_buff, sizeof(gps2_buff), "GPS -> nbsat : %d ; fixlvl : %d ; speed : %lf ",
                     ha_gps_packet_ptr->satUsed, ha_gps_packet_ptr->quality, ha_gps_packet_ptr->groundSpeed);
        } else {
            snprintf(gps1_buff, sizeof(gps1_buff), "GPS -> lat : N/A ; lon : N/A ; alt : N/A");
            snprintf(gps2_buff, sizeof(gps2_buff), "GPS -> lnbsat : N/A ; fixlvl : N/A ; speed : N/A");
        }

        // test

        snprintf(info, sizeof(info), "POSX -> %f || POSY -> %f || distRoueGauche -> %f || distRoueDroite -> %f", posX,
                 posY, distRoueGauche, distRoueDroite);
        snprintf(info2, sizeof(info2), "teta -> %f || time -> %d", teta, fakeTime);
        draw_text(info, 10, 460);
        draw_text(info2, 10, 470);
        // test
        draw_text(gyro_buff, 10, 410);
        draw_text(accel_buff, 10, 420);
        draw_text(odo_buff, 10, 430);
        draw_text(gps1_buff, 10, 440);
        draw_text(gps2_buff, 10, 450);

        tic_detection(ha_odo_packet_ptr);

        // ##############################################
        ApiPostPacketPtr api_post_packet_ptr = nullptr;

        api_post_packet_ptr_access_.lock();
        api_post_packet_ptr = api_post_packet_ptr_;
        api_post_packet_ptr_access_.unlock();

        if (api_post_packet_ptr != nullptr) {
            for (uint i = 0; i < api_post_packet_ptr->postList.size(); i++) {
                if (api_post_packet_ptr->postList[i].postType == ApiPostPacket::PostType::RED) {
                    draw_red_post(static_cast<int>( api_post_packet_ptr->postList[i].x * 100.0 ),
                                  static_cast<int>( api_post_packet_ptr->postList[i].y * 100.0 ));
                }
            }
        }

        // ##############################################

        static int flying_pixel_x = 0;

        if (flying_pixel_x > 800) {
            flying_pixel_x = 0;
        }

        SDL_SetRenderDrawColor(renderer_, 200, 150, 125, 255);
        SDL_Rect flying_pixel;
        flying_pixel.w = 1;
        flying_pixel.h = 1;
        flying_pixel.y = 482;
        flying_pixel.x = flying_pixel_x;

        flying_pixel_x++;

        SDL_RenderFillRect(renderer_, &flying_pixel);

        SDL_RenderPresent(renderer_);

        // compute wait time
        milliseconds end_ms = duration_cast<milliseconds>(system_clock::now().time_since_epoch());
        int64_t end_now = static_cast<int64_t>( end_ms.count());
        int64_t wait_time = nextTick - end_now;

        if (wait_time <= 0) {
            wait_time = 10;
        }

        //std::cout << "display time took " << display_time << " ms so wait_time is " << wait_time << " ms " << std::endl;

        // repeat keyboard reading for smoother command inputs
        readSDLKeyboard();
        manageSDLKeyboard();

        std::this_thread::sleep_for(std::chrono::milliseconds(wait_time / 2));

        readSDLKeyboard();
        manageSDLKeyboard();

        std::this_thread::sleep_for(std::chrono::milliseconds(wait_time / 2));
    }

    threadStarted_ = false;
    stopThreadAsked_ = false;

    exitSDL();

    std::cout << "Stopping main thread." << std::endl;
}

// #################################################
//
void Core::draw_text(char buffer[100], int x, int y) {
    SDL_Surface *surfaceMessageAccel = TTF_RenderText_Solid(ttf_font_, buffer, sdl_color_white_);
    SDL_Texture *messageAccel = SDL_CreateTextureFromSurface(renderer_, surfaceMessageAccel);

    SDL_FreeSurface(surfaceMessageAccel);

    SDL_Rect message_rect_accel;
    message_rect_accel.x = x;
    message_rect_accel.y = y;

    SDL_QueryTexture(messageAccel, NULL, NULL, &message_rect_accel.w, &message_rect_accel.h);
    SDL_RenderCopy(renderer_, messageAccel, NULL, &message_rect_accel);

    SDL_DestroyTexture(messageAccel);
}

// #################################################
//
void Core::draw_lidar(uint16_t lidar_distance_[271]) {
    zoneDetection_gauche = 0;
    zoneDetection_milieu = 0;
    zoneDetection_droite = 0;
    detectionObject_gauche = false;
    detectionObject_milieu = false;
    detectionObject_droite = false;
    for (int i = 0; i < 271; i++) {
        double dist = static_cast<double>( lidar_distance_[i] ) / 10.0f;

        if (dist < 3.0f) {
            dist = 5000.0f;
        }

        if (i > 45 && i < 226) {
            double x_cos = dist * cos(static_cast<double>((i - 45) * M_PI / 180. ));
            double y_sin = dist * sin(static_cast<double>((i - 45) * M_PI / 180. ));

            double x = 400.0 - x_cos;
            double y = 400.0 - y_sin;

            SDL_SetRenderDrawColor(renderer_, 255, 255, 255, 255);
            SDL_Rect lidar_pixel;

            lidar_pixel.w = 1;
            lidar_pixel.h = 1;
            lidar_pixel.x = static_cast<int>( x );
            lidar_pixel.y = static_cast<int>( y );

            SDL_RenderFillRect(renderer_, &lidar_pixel);

            if (i > 80 && i <= 120) {
                zoneDetection_gauche += dist;
                if ((i % 10) == 0) {
                    if ((zoneDetection_gauche / PAS) < DETECTION) {
                        detectionObject_gauche = true;
                    }
                    zoneDetection_gauche = 0;
                }
            }

            if (i > 120 && i <= 160) {
                zoneDetection_milieu += dist;
                if ((i % 10) == 0) {
                    if ((zoneDetection_milieu / PAS) < DETECTION) {
                        detectionObject_milieu = true;
                    }
                    zoneDetection_milieu = 0;
                }
            }

            if (i > 160 && i <= 200) {
                zoneDetection_droite += dist;
                if ((i % 10) == 0) {
                    if ((zoneDetection_droite / PAS) < DETECTION) {
                        detectionObject_droite = true;
                    }
                    zoneDetection_droite = 0;
                }
            }
        }
    }


}

// #################################################
//
void Core::draw_red_post(int x, int y) {
    SDL_SetRenderDrawColor(renderer_, 255, 0, 0, 255);
    SDL_Rect rp;
    rp.w = 2;
    rp.h = 2;
    rp.y = 400 - x - 1;
    rp.x = 400 - y - 1;

    SDL_RenderFillRect(renderer_, &rp);
}

// #################################################
//
void Core::draw_robot() {
    SDL_SetRenderDrawColor(renderer_, 200, 200, 200, 255);
    SDL_Rect main;
    main.w = 42;
    main.h = 80;
    main.y = 480 - main.h;
    main.x = 400 - (main.w / 2);

    SDL_RenderFillRect(renderer_, &main);

    SDL_SetRenderDrawColor(renderer_, 100, 100, 100, 255);
    SDL_Rect flw;
    flw.w = 8;
    flw.h = 20;
    flw.y = 480 - 75;
    flw.x = 400 - 21;

    SDL_RenderFillRect(renderer_, &flw);

    SDL_SetRenderDrawColor(renderer_, 100, 100, 100, 255);
    SDL_Rect frw;
    frw.w = 8;
    frw.h = 20;
    frw.y = 480 - 75;
    frw.x = 400 + 21 - 8;

    SDL_RenderFillRect(renderer_, &frw);

    SDL_SetRenderDrawColor(renderer_, 100, 100, 100, 255);
    SDL_Rect rlw;
    rlw.w = 8;
    rlw.h = 20;
    rlw.y = 480 - 5 - 20;
    rlw.x = 400 - 21;

    SDL_RenderFillRect(renderer_, &rlw);

    SDL_SetRenderDrawColor(renderer_, 100, 100, 100, 255);
    SDL_Rect rrw;
    rrw.w = 8;
    rrw.h = 20;
    rrw.y = 480 - 5 - 20;
    rrw.x = 400 + 21 - 8;

    SDL_RenderFillRect(renderer_, &rrw);

    SDL_SetRenderDrawColor(renderer_, 120, 120, 120, 255);
    SDL_Rect lidar;
    lidar.w = 8;
    lidar.h = 8;
    lidar.y = 480 - 80 - 8;
    lidar.x = 400 - 4;

    SDL_RenderFillRect(renderer_, &lidar);
}

// #################################################
//
void Core::draw_images() {
    SDL_Surface *left_image;

    SDL_Surface *right_image;

#if SDL_BYTEORDER == SDL_BIG_ENDIAN
    Uint32 rmask = 0xff000000;
    Uint32 gmask = 0x00ff0000;
    Uint32 bmask = 0x0000ff00;
    Uint32 amask = 0x000000ff;
#else
    Uint32 rmask = 0x000000ff;
    Uint32 gmask = 0x0000ff00;
    Uint32 bmask = 0x00ff0000;
    Uint32 amask = 0xff000000;
#endif

    last_images_buffer_access_.lock();

    if (last_image_type_ == ApiStereoCameraPacket::ImageType::RAW_IMAGES or
        last_image_type_ == ApiStereoCameraPacket::ImageType::RAW_IMAGES_ZLIB) {
        left_image = SDL_CreateRGBSurfaceFrom(last_images_buffer_, 752, 480, 3 * 8, 752 * 3, rmask, gmask, bmask,
                                              amask);
        right_image = SDL_CreateRGBSurfaceFrom(last_images_buffer_ + (752 * 480 * 3), 752, 480, 3 * 8, 752 * 3, rmask,
                                               gmask, bmask, amask);
    } else {
        left_image = SDL_CreateRGBSurfaceFrom(last_images_buffer_, 376, 240, 3 * 8, 376 * 3, rmask, gmask, bmask,
                                              amask);
        right_image = SDL_CreateRGBSurfaceFrom(last_images_buffer_ + (376 * 240 * 3), 376, 240, 3 * 8, 376 * 3, rmask,
                                               gmask, bmask, amask);
    }

    last_images_buffer_access_.unlock();

    SDL_Rect left_rect = {400 - 376 - 10, 485, 376, 240};

    SDL_Rect right_rect = {400 + 10, 485, 376, 240};

    SDL_Texture *left_texture = SDL_CreateTextureFromSurface(renderer_, left_image);

    SDL_Texture *right_texture = SDL_CreateTextureFromSurface(renderer_, right_image);

    SDL_RenderCopy(renderer_, left_texture, NULL, &left_rect);

    SDL_RenderCopy(renderer_, right_texture, NULL, &right_rect);
}

// #################################################
//
SDL_Window *
Core::initSDL(const char *name, int szX, int szY) {
    std::cout << "Init SDL";

    SDL_Window *screen;
    std::cout << ".";

    SDL_Init(SDL_INIT_EVERYTHING);
    std::cout << ".";

    screen = SDL_CreateWindow(name, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, szX, szY, SDL_WINDOW_SHOWN);
    std::cout << ".";

    renderer_ = SDL_CreateRenderer(screen, 0, SDL_RENDERER_ACCELERATED);
    std::cout << ".";

    TTF_Init();
    std::cout << ".";

    // Set render color to black ( background will be rendered in this color )
    SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
    std::cout << ".";

    SDL_RenderClear(renderer_);
    std::cout << ".";

    sdl_color_red_ = {255, 0, 0, 0};
    sdl_color_white_ = {255, 255, 255, 0};
    ttf_font_ = TTF_OpenFont("mono.ttf", 12);

    if (ttf_font_ == nullptr) {
        std::cerr << "Failed to load SDL Font! Error: " << TTF_GetError() << '\n';
    }

    std::cout << "DONE" << std::endl;

    return screen;
}

// #################################################
//
void
Core::exitSDL() {
    SDL_Quit();
}

// #################################################
//
void
Core::readSDLKeyboard() {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
            // Cas d'une touche enfoncée
            case SDL_KEYDOWN:
                sdlKey_[event.key.keysym.scancode] = 1;
                break;
                // Cas d'une touche relâchée
            case SDL_KEYUP:
                sdlKey_[event.key.keysym.scancode] = 0;
                break;

            case SDL_MOUSEBUTTONDOWN:
                SDL_GetMouseState(&mouse_pos_x, &mouse_pos_y);
                for (int i = 0; i < 10; i++) {
                    SDL_Rect box = buttons[i];
                    if (mouse_pos_x > box.x
                        && mouse_pos_x < box.x + box.w
                        && mouse_pos_y > box.y
                        && mouse_pos_y < box.y + box.h) {
                        button_selected = i;
                        command_interface = true;
                    }
                }
                break;
            case SDL_MOUSEBUTTONUP:
                command_interface = false;
                break;
            default:
                break;
        }
    }
}

// #################################################
//
bool
Core::manageSDLKeyboard() {
    bool keyPressed = false;

    int8_t left = 0;
    int8_t right = 0;

    if (sdlKey_[SDL_SCANCODE_ESCAPE] == 1) {
        stopThreadAsked_ = true;
        return true;
    }

    if (sdlKey_[SDL_SCANCODE_O] == 1) {
        asked_start_video_ = true;
    }

    if (sdlKey_[SDL_SCANCODE_F] == 1) {
        asked_stop_video_ = true;
    }

    if (sdlKey_[SDL_SCANCODE_UP] == 1 and sdlKey_[SDL_SCANCODE_LEFT] == 1) {
        if (!detectionObject_gauche && !detectionObject_milieu) {
            left = 32;
            right = 63;
            keyPressed = true;
        }
    } else if (sdlKey_[SDL_SCANCODE_UP] == 1 and sdlKey_[SDL_SCANCODE_RIGHT] == 1) {
        if (!detectionObject_droite && !detectionObject_milieu) {
            left = 63;
            right = 32;
            keyPressed = true;
        }

    } else if (sdlKey_[SDL_SCANCODE_DOWN] == 1 and sdlKey_[SDL_SCANCODE_LEFT] == 1) {
        if (!detectionObject_gauche) {
            left = -32;
            right = -63;
            keyPressed = true;
        }
    } else if (sdlKey_[SDL_SCANCODE_DOWN] == 1 and sdlKey_[SDL_SCANCODE_RIGHT] == 1) {
        if (!detectionObject_droite) {
            left = -63;
            right = -32;
            keyPressed = true;
        }

    } else if (sdlKey_[SDL_SCANCODE_UP] == 1) {
        if (!detectionObject_milieu) {
            left = 63;
            right = 63;
            keyPressed = true;
        }
    } else if (sdlKey_[SDL_SCANCODE_DOWN] == 1) {
        left = -63;
        right = -63;
        keyPressed = true;

    } else if (sdlKey_[SDL_SCANCODE_LEFT] == 1) {
        if (!detectionObject_gauche) {
            left = -63;
            right = 63;
            keyPressed = true;
        }
    } else if (sdlKey_[SDL_SCANCODE_RIGHT] == 1) {
        if (!detectionObject_droite) {
            left = 63;
            right = -63;
            keyPressed = true;
        }
    } else if (command_interface && !mode_automatique) {
        SDL_GetMouseState(&mouse_pos_x, &mouse_pos_y);
        SDL_Rect box = buttons[button_selected];
        if (mouse_pos_x > box.x
            && mouse_pos_x < box.x + box.w
            && mouse_pos_y > box.y
            && mouse_pos_y < box.y + box.h) {
            switch (button_selected) {
                case 0: // Up
                    if (!detectionObject_milieu) {
                        left = 10;
                        right = 10;
                    }
                    break;
                case 1: // Left
                    if (!detectionObject_gauche) {
                        left = 10;
                        right = 63;
                    }
                    break;
                case 2: // Right
                    if (!detectionObject_droite) {
                        left = 63;
                        right = 10;
                    }
                    break;
                case 3: // Down
                    left = -63;
                    right = -63;
                    break;
                case 4: // Automatique
                    mode_automatique = true;
                    pos_init = dist_rl;
                    printf("Mode automatique\npos_init: %f", pos_init);
                    break;
                case 5: // Reculer
                    break;
                case 6: // +
                    distance_a_parcourir += 6.454;
                    break;
                case 7: // -
                    distance_a_parcourir -= 6.454;
                    if (distance_a_parcourir < 0.0)
                        distance_a_parcourir = 0.0;
                    break;
                case 8:
                    largeur_culture += 6.454;
                    break;
                case 9:
                    largeur_culture -= 6.454;
                    if (largeur_culture <= 0.0)
                        largeur_culture = 0.0;
                default:
                    break;
            }
        } else {
            printf("No one button selected");
        }
    }
    // COMMANDE MOTEUR
    last_motor_access_.lock();
    last_left_motor_ = static_cast<int8_t >( left * 2 );
    last_right_motor_ = static_cast<int8_t >( right * 2 );
    last_motor_access_.unlock();

    // deplacement d'un longeur de rangée
    if (mode_automatique && (dist_rl < pos_init + distance_a_parcourir) && range1) {
        if (!detectionObject_milieu) {
            printf("Mode automatique: Objet non detecte Longueur rangée\n");
            deplacement(1);
        } else {
            printf("Mode automatique: Objet detecte\n");
        }
        vir1 = true;
    }
        // debut demi tour
        //premier virage
    else if (mode_automatique && (virage_var < 200) && vir1) {
        range1 = false;
        if (!detectionObject_milieu) {
            printf("Mode automatique: Objet non detecte virage\n");
            virage('g');
            virage_var++;
        } else {
            printf("Mode automatique: Objet detecte\n");
        }
            range2 = true;
            post_demi = dist_rl;
        //marche arriere
    } else if (mode_automatique && (marche_arriere < 400) && range2) {
        vir1 = false;
        virage_var = 0;
        if (!detectionObject_milieu) {
            printf("Mode automatique: Objet non detecte Longueur rangée\n");
            deplacement(-1);
            marche_arriere++;
        } else {
            printf("Mode automatique: Objet detecte\n");
        }
        vir2 = true;
        //deuxieme virage
    } else if (mode_automatique && (virage_var < 200) && vir2) {
        range2 = false;
        if (!detectionObject_milieu) {
            printf("Mode automatique: Objet non detecte virage\n");
            virage('g');
            virage_var++;
        } else {
            printf("Mode automatique: Objet detecte\n");
        }
            range1 = true;
            post_demi = dist_rl;
        // retour
    } else if (mode_automatique && (dist_rl < post_demi + distance_a_parcourir) && range1) {
        vir1 = false;
        if (!detectionObject_milieu) {
            printf("Mode automatique: Objet non detecte Longueur rangée\n");
            deplacement(1);
        } else {
            printf("Mode automatique: Objet detecte\n");
        }

    } else {
        mode_automatique = false;
        range1 = true;
        virage_var = 0;
    }


    return
            keyPressed;
}

// #################################################
//
void
Core::manageReceivedPacket(BaseNaio01PacketPtr packetPtr) {
    //std::cout << "Packet received id : " << static_cast<int>( packetPtr->getPacketId() ) << std::endl;

    if (std::dynamic_pointer_cast<HaLidarPacket>(packetPtr)) {
        HaLidarPacketPtr haLidarPacketPtr = std::dynamic_pointer_cast<HaLidarPacket>(packetPtr);

        ha_lidar_packet_ptr_access_.lock();
        ha_lidar_packet_ptr_ = haLidarPacketPtr;
        ha_lidar_packet_ptr_access_.unlock();
    } else if (std::dynamic_pointer_cast<HaGyroPacket>(packetPtr)) {
        HaGyroPacketPtr haGyroPacketPtr = std::dynamic_pointer_cast<HaGyroPacket>(packetPtr);

        ha_gyro_packet_ptr_access_.lock();
        ha_gyro_packet_ptr_ = haGyroPacketPtr;
        ha_gyro_packet_ptr_access_.unlock();
    } else if (std::dynamic_pointer_cast<HaAcceleroPacket>(packetPtr)) {
        HaAcceleroPacketPtr haAcceleroPacketPtr = std::dynamic_pointer_cast<HaAcceleroPacket>(packetPtr);

        ha_accel_packet_ptr_access_.lock();
        ha_accel_packet_ptr_ = haAcceleroPacketPtr;
        ha_accel_packet_ptr_access_.unlock();
    } else if (std::dynamic_pointer_cast<HaOdoPacket>(packetPtr)) {
        HaOdoPacketPtr haOdoPacketPtr = std::dynamic_pointer_cast<HaOdoPacket>(packetPtr);

        ha_odo_packet_ptr_access.lock();
        ha_odo_packet_ptr_ = haOdoPacketPtr;
        ha_odo_packet_ptr_access.unlock();
    } else if (std::dynamic_pointer_cast<ApiPostPacket>(packetPtr)) {
        ApiPostPacketPtr apiPostPacketPtr = std::dynamic_pointer_cast<ApiPostPacket>(packetPtr);

        api_post_packet_ptr_access_.lock();
        api_post_packet_ptr_ = apiPostPacketPtr;
        api_post_packet_ptr_access_.unlock();
    } else if (std::dynamic_pointer_cast<HaGpsPacket>(packetPtr)) {
        HaGpsPacketPtr haGpsPacketPtr = std::dynamic_pointer_cast<HaGpsPacket>(packetPtr);

        ha_gps_packet_ptr_access_.lock();
        ha_gps_packet_ptr_ = haGpsPacketPtr;
        ha_gps_packet_ptr_access_.unlock();
    } else if (std::dynamic_pointer_cast<ApiStereoCameraPacket>(packetPtr)) {
        ApiStereoCameraPacketPtr api_stereo_camera_packet_ptr = std::dynamic_pointer_cast<ApiStereoCameraPacket>(
                packetPtr);

        milliseconds now_ms = duration_cast<milliseconds>(system_clock::now().time_since_epoch());
        last_image_received_time_ = static_cast<int64_t>( now_ms.count());

        api_stereo_camera_packet_ptr_access_.lock();
        api_stereo_camera_packet_ptr_ = api_stereo_camera_packet_ptr;
        api_stereo_camera_packet_ptr_access_.unlock();
    }

}

// #################################################
//
void
Core::joinMainThread() {
    graphicThread_.join();
}

// #################################################
//
void Core::joinServerReadThread() {
    serverReadThread_.join();
}

// #################################################
//
void Core::image_server_thread() {
    imageServerReadthreadStarted_ = false;
    imageServerWriteThreadStarted_ = false;

    stopImageServerReadThreadAsked_ = false;
    stopImageServerWriteThreadAsked_ = false;

    stopImageServerThreadAsked_ = false;
    imageServerThreadStarted_ = true;

    struct sockaddr_in imageServer;

    //Create socket
    image_socket_desc_ = socket(AF_INET, SOCK_STREAM, 0);

    if (image_socket_desc_ == -1) {
        std::cout << "Could not create socket" << std::endl;
    }

    imageServer.sin_addr.s_addr = inet_addr(hostAdress_.c_str());
    imageServer.sin_family = AF_INET;
    imageServer.sin_port = htons(static_cast<uint16_t>( hostPort_ + 2 ));

    //Connect to remote server
    if (connect(image_socket_desc_, (struct sockaddr *) &imageServer, sizeof(imageServer)) < 0) {
        puts("image connect error");
    } else {
        puts("Connected image\n");
        imageSocketConnected_ = true;
    }

    image_prepared_thread_ = std::thread(&Core::image_preparer_thread, this);

    std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<int64_t>( 50 )));

    imageServerReadThread_ = std::thread(&Core::image_server_read_thread, this);

    std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<int64_t>( 50 )));

    imageServerWriteThread_ = std::thread(&Core::image_server_write_thread, this);

    std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<int64_t>( 50 )));

    while (not stopImageServerThreadAsked_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<int64_t>( 500 )));
    }

    imageServerThreadStarted_ = false;
    stopImageServerThreadAsked_ = false;
}

// #################################################
//
void Core::image_server_read_thread() {
    imageServerReadthreadStarted_ = true;

    uint8_t receiveBuffer[4000000];

    while (!stopImageServerReadThreadAsked_) {
        // any time : read incoming messages.
        int readSize = (int) read(image_socket_desc_, receiveBuffer, 4000000);

        if (readSize > 0) {
            bool packetHeaderDetected = false;

            bool atLeastOnePacketReceived = imageNaioCodec_.decode(receiveBuffer, static_cast<uint>( readSize ),
                                                                   packetHeaderDetected);

            // manage received messages
            if (atLeastOnePacketReceived) {
                for (auto &&packetPtr : imageNaioCodec_.currentBasePacketList) {
                    if (std::dynamic_pointer_cast<ApiStereoCameraPacket>(packetPtr)) {
                        ApiStereoCameraPacketPtr api_stereo_camera_packet_ptr = std::dynamic_pointer_cast<ApiStereoCameraPacket>(
                                packetPtr);

                        milliseconds now_ms = duration_cast<milliseconds>(system_clock::now().time_since_epoch());
                        last_image_received_time_ = static_cast<int64_t>( now_ms.count());

                        api_stereo_camera_packet_ptr_access_.lock();
                        api_stereo_camera_packet_ptr_ = api_stereo_camera_packet_ptr;
                        api_stereo_camera_packet_ptr_access_.unlock();
                    }
                }

                imageNaioCodec_.currentBasePacketList.clear();
            }
        }

        std::this_thread::sleep_for(
                std::chrono::milliseconds(static_cast<int64_t>( WAIT_SERVER_IMAGE_TIME_RATE_MS )));
    }

    imageServerReadthreadStarted_ = false;
    stopImageServerReadThreadAsked_ = false;
}

// #################################################
// use only for server socket watchdog
void Core::image_server_write_thread() {
    imageServerWriteThreadStarted_ = true;

    while (!stopImageServerWriteThreadAsked_) {
        if (imageSocketConnected_) {
            ApiWatchdogPacketPtr api_watchdog_packet_ptr = std::make_shared<ApiWatchdogPacket>(42);

            cl_copy::BufferUPtr buffer = api_watchdog_packet_ptr->encode();

            write(image_socket_desc_, buffer->data(), buffer->size());
        }

        std::this_thread::sleep_for(
                std::chrono::milliseconds(static_cast<int64_t>( IMAGE_SERVER_WATCHDOG_SENDING_RATE_MS )));
    }

    imageServerWriteThreadStarted_ = false;
    stopImageServerWriteThreadAsked_ = false;
}

// #################################################
//
void Core::image_preparer_thread() {
    Bytef zlibUncompressedBytes[4000000l];

    while (true) {
        std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<int64_t>( IMAGE_PREPARING_RATE_MS )));

        ApiStereoCameraPacketPtr api_stereo_camera_packet_ptr = nullptr;

        api_stereo_camera_packet_ptr_access_.lock();

        if (api_stereo_camera_packet_ptr_ != nullptr) {
            last_image_type_ = api_stereo_camera_packet_ptr_->imageType;

            api_stereo_camera_packet_ptr = api_stereo_camera_packet_ptr_;

            api_stereo_camera_packet_ptr_ = nullptr;
        }

        api_stereo_camera_packet_ptr_access_.unlock();

        if (api_stereo_camera_packet_ptr != nullptr) {
            cl_copy::BufferUPtr bufferUPtr = std::move(api_stereo_camera_packet_ptr->dataBuffer);

            if (last_image_type_ == ApiStereoCameraPacket::ImageType::RAW_IMAGES_ZLIB or
                last_image_type_ == ApiStereoCameraPacket::ImageType::RECTIFIED_COLORIZED_IMAGES_ZLIB) {
                uLong sizeDataUncompressed = 0l;

                uncompress((Bytef *) zlibUncompressedBytes, &sizeDataUncompressed, bufferUPtr->data(),
                           static_cast<uLong>( bufferUPtr->size()));

                last_images_buffer_access_.lock();

                if (last_image_type_ == ApiStereoCameraPacket::ImageType::RAW_IMAGES_ZLIB) {
                    // don't know how to display 8bits image with sdl...
                    for (uint i = 0; i < sizeDataUncompressed; i++) {
                        last_images_buffer_[(i * 3) + 0] = zlibUncompressedBytes[i];
                        last_images_buffer_[(i * 3) + 1] = zlibUncompressedBytes[i];
                        last_images_buffer_[(i * 3) + 2] = zlibUncompressedBytes[i];
                    }
                } else {
                    for (uint i = 0; i < sizeDataUncompressed; i++) {
                        last_images_buffer_[i] = zlibUncompressedBytes[i];
                    }
                }

                last_images_buffer_access_.unlock();
            } else {
                last_images_buffer_access_.lock();

                if (last_image_type_ == ApiStereoCameraPacket::ImageType::RAW_IMAGES) {
                    // don't know how to display 8bits image with sdl...
                    for (uint i = 0; i < bufferUPtr->size(); i++) {
                        last_images_buffer_[(i * 3) + 0] = bufferUPtr->at(i);
                        last_images_buffer_[(i * 3) + 1] = bufferUPtr->at(i);
                        last_images_buffer_[(i * 3) + 2] = bufferUPtr->at(i);
                    }
                } else {
                    for (uint i = 0; i < bufferUPtr->size(); i++) {
                        last_images_buffer_[i] = bufferUPtr->at(i);
                    }
                }

                last_images_buffer_access_.unlock();
            }
        } else {
            milliseconds now_ms = duration_cast<milliseconds>(system_clock::now().time_since_epoch());
            int64_t now = static_cast<int64_t>( now_ms.count());

            int64_t diff_time = now - last_image_received_time_;

            if (diff_time > TIME_BEFORE_IMAGE_LOST_MS) {
                last_image_received_time_ = now;

                uint8_t fake = 0;

                last_images_buffer_access_.lock();

                for (int i = 0; i < 721920 * 3; i++) {
                    if (fake >= 255) {
                        fake = 0;
                    }

                    last_images_buffer_[i] = fake;

                    fake++;
                }

                last_images_buffer_access_.unlock();
            }
        }
    }
}

// #################################################
//
//COMMANDE DU ROBOT
void Core::server_write_thread() {
    stopServerWriteThreadAsked_ = false;
    serverWriteThreadStarted_ = true;

    for (int i = 0; i < 100; i++) {
        ApiMotorsPacketPtr first_packet = std::make_shared<ApiMotorsPacket>(0, 0);
        cl_copy::BufferUPtr first_buffer = first_packet->encode();
        write(socket_desc_, first_buffer->data(), first_buffer->size());
    }

    while (not stopServerWriteThreadAsked_) {
        //direction calculation
        if (last_left_motor_ > 0 && last_right_motor_ > 0) {
            dir_f = true;
            dir_r = false;
        }
        if (last_left_motor_ < 0 && last_right_motor_ < 0) {
            dir_f = false;
            dir_r = true;
        }
        if (last_left_motor_ == 0 || last_right_motor_ == 0) {
            dir_f = false;
            dir_r = false;
        }
        last_motor_access_.lock();
        //Si je détecte beaucoup de point alors
        if (detectionObject_droite || detectionObject_milieu || detectionObject_gauche) {
            //arrêt du robot
            // COMMANDE MOTEUR
            //last_motor_access_.lock();
            last_left_motor_ = static_cast<int8_t >(0);
            last_right_motor_ = static_cast<int8_t >(0);
            //last_motor_access_.unlock();
            printf("OBJECT DETECTED\n");
        }
        HaMotorsPacketPtr haMotorsPacketPtr = std::make_shared<HaMotorsPacket>(last_left_motor_, last_right_motor_);

        last_motor_access_.unlock();

        sendPacketListAccess_.lock();

        sendPacketList_.push_back(haMotorsPacketPtr);

        for (auto &&packet : sendPacketList_) {
            cl_copy::BufferUPtr buffer = packet->encode();

            int sentSize = (int) write(socket_desc_, buffer->data(), buffer->size());

            (void) sentSize;
        }

        sendPacketList_.clear();

        sendPacketListAccess_.unlock();

        std::this_thread::sleep_for(std::chrono::milliseconds(SERVER_SEND_COMMAND_RATE_MS));
    }

    stopServerWriteThreadAsked_ = false;
    serverWriteThreadStarted_ = false;
}

void Core::draw_button(int posX, int posY, int width, int height) {

    SDL_SetRenderDrawColor(renderer_, 200, 200, 200, 255);
    SDL_Rect bouton;

    bouton.w = width;
    bouton.h = height;
    bouton.y = posY;
    bouton.x = posX;

    SDL_RenderFillRect(renderer_, &bouton);
}

void Core::calc_info() {

    while (!stopThreadAsked_) {

        if ((last_left_motor_ > 0 || last_right_motor_ > 0)) {
            info_robot.lock();
            //CALCUL
            double distanceRoueGauche = getDistRoueGauche() + dist_rl;
            double distanceRoueDroite = getDistRoueDroite() + dist_rr;

            double tetaa = (distanceRoueGauche - distanceRoueDroite) / ENTRAXE;

            double vitesseGauche = distanceRoueGauche - getDistRoueGauche();
            double vitesseDroite = distanceRoueDroite - getDistRoueDroite();
            double vitesseMoyenne = (vitesseGauche + vitesseDroite) / 2;

            double x = getPosX() + vitesseMoyenne * RAYON * cos(tetaa);
            double y = getPosY() + vitesseMoyenne * RAYON * sin(tetaa);

            //UPDATE
            setDistRoueDroite(distanceRoueDroite);
            setDistRoueGauche(distanceRoueGauche);

            setTeta(tetaa);
            setPosX(x);
            setPosY(y);
            info_robot.unlock();
        }
        setFakeTime(getFakeTime() + 1);
        sleep(1);
    }

}


void Core::draw_command_interface(int posX, int posY) {
    int w_button = 35, h_button = 35;
    int w_button_auto = 80, h_button_auto = 30;

    buttons[0] = {posX + 25, posY, w_button, h_button};
    buttons[1] = {posX, posY + 40, w_button, h_button};
    buttons[2] = {posX + 50, posY + 40, w_button, h_button};
    buttons[3] = {posX + 25, posY + 80, w_button, h_button};
    buttons[4] = {posX, posY + 120, w_button_auto, h_button_auto};
    buttons[5] = {posX, posY + 160, w_button_auto, h_button_auto};
    buttons[6] = {posX + w_button_auto + 30, posY + 90, w_button, h_button};
    buttons[7] = {posX + w_button_auto + 60 + w_button, posY + 90, w_button, h_button};
    buttons[8] = {posX + w_button_auto + 30, posY + 200, w_button, h_button};
    buttons[9] = {posX + w_button_auto + 60 + w_button, posY + 200, w_button, h_button};


    // Direction pad
    draw_button(buttons[0].x, buttons[0].y, buttons[0].w, buttons[0].h);
    draw_text("Up", posX + 35, posY + 10);

    draw_button(buttons[1].x, buttons[1].y, buttons[1].w, buttons[1].h);
    draw_text("Left", posX + 5, posY + 50);

    draw_button(buttons[2].x, buttons[2].y, buttons[2].w, buttons[2].h);
    draw_text("Right", posX + 55, posY + 50);

    draw_button(buttons[3].x, buttons[3].y, buttons[3].w, buttons[3].h);
    draw_text("Down", posX + 30, posY + 85);

    // Button auto
    draw_button(buttons[4].x, buttons[4].y, buttons[4].w, buttons[4].h);
    draw_button(buttons[5].x, buttons[5].y, buttons[5].w, buttons[5].h);

    // Text
    draw_text("Automatique", posX + 10, posY + 130);
    draw_text("Reculer", posX + 10, posY + 170);

    // Informations
    char text_distance_a_parcourir[50];
    char text_largeur_culture[50];
    char text_distance[50];
    char text_angle[50];
    char text_posX[50];
    char text_posY[50];

    ///////// variable temporaire de test /////////
    double angle = 45.654654;
    double distance = 600.984651;
    double posXtest = 10.984561;
    double posYtest = 65.984654;

    // mise du test en buffer pour affichage dynamique
    snprintf(text_distance, sizeof(text_distance), "Distance parcourue: %7.3f", distance);
    snprintf(text_angle, sizeof(text_angle), "Angle: %7.3f", angle);
    snprintf(text_posX, sizeof(text_posX), "Pos x: %7.3f", posXtest);
    snprintf(text_posY, sizeof(text_posY), "Pos y: %7.3f", posYtest);

    draw_text(text_distance, posX + 110, posY);
    draw_text(text_angle, posX + 110, posY + 20);
    draw_text(text_posX, posX + 110, posY + 40);
    draw_text(text_posY, posX + 110, posY + 60);

    // +/- button
    draw_button(buttons[6].x, buttons[6].y, buttons[6].w, buttons[6].h);
    draw_text("+", posX + w_button_auto + 45, posY + 100);
    draw_button(buttons[7].x, buttons[7].y, buttons[7].w, buttons[7].h);
    draw_text("-", posX + w_button_auto + 75 + w_button, posY + 100);

    draw_button(buttons[8].x, buttons[8].y, buttons[8].w, buttons[8].h);
    draw_text("+", posX + w_button_auto + 45, posY + 210);
    draw_button(buttons[9].x, buttons[9].y, buttons[9].w, buttons[9].h);
    draw_text("-", posX + w_button_auto + 75 + w_button, posY + 210);

//	// Text
    snprintf(text_distance_a_parcourir, sizeof(text_distance_a_parcourir), "Longeur de la rangee: %.3f",
             distance_a_parcourir);
    draw_text(text_distance_a_parcourir, posX + w_button_auto + 30, posY + 140);
    draw_text("Distance parcourue: ", posX + w_button_auto + 30, posY + 160);

    // text de la largeur de la rangée
    snprintf(text_largeur_culture, sizeof(text_largeur_culture), "Largeur de la rangee: %.3f", largeur_culture);
    draw_text(text_largeur_culture, posX + w_button_auto + 30, posY + 240);
//	char text_walk_distance [50];
//	//variable temporaire de test
//	double distanceAuto = 6.465 * 25;
//
//	snprintf( text_walk_distance, sizeof( text_walk_distance ), "Distance parcourue: %7.3f", distanceAuto) ;
//	draw_text(text_walk_distance, posX + w_button_auto + 30, posY + 170);
    //tic_detection();
    if (ha_odo_packet_ptr_ == nullptr) {
        draw_text("no value", posX + w_button_auto + 30, posY + 170);
    } else {
        char vdbl1[150];
        sprintf(vdbl1, "%.3f", dist_rl);
        strcat(vdbl1, " Left");
        draw_text(vdbl1, posX + w_button_auto + 30, posY + 170);

    }

    if (ha_odo_packet_ptr_ == nullptr) {
        draw_text("no value", posX + w_button_auto + 30, posY + 180);
    } else {
        char vdbl2[150];
        sprintf(vdbl2, "%.3f", dist_rr);
        strcat(vdbl2, " Right");
        draw_text(vdbl2, posX + w_button_auto + 30, posY + 180);
    }
}

double Core::getPosX() const {
    return posX;
}

void Core::setPosX(double possX) {
    Core::posX = possX;
}

double Core::getPosY() const {
    return posY;
}

void Core::setPosY(double possY) {
    Core::posY = possY;
}

double Core::getDistRoueGauche() const {
    return distRoueGauche;
}

void Core::setDistRoueGauche(double distRoueGauchee) {
    Core::distRoueGauche = distRoueGauchee;
}

double Core::getDistRoueDroite() const {
    return distRoueDroite;
}

void Core::setDistRoueDroite(double distRoueDroitee) {
    Core::distRoueDroite = distRoueDroitee;
}

double Core::getTeta() const {
    return teta;
}

void Core::setTeta(double tetaa) {
    Core::teta = tetaa;
}

int Core::getFakeTime() const {
    return fakeTime;
}

void Core::setFakeTime(int timefake) {
    Core::fakeTime = timefake;
}

void Core::tic_detection(HaOdoPacketPtr hod) {
//    cout << "tdeb" << endl;
    if ((last_left_motor_ > 0 || last_right_motor_ > 0) && hod != NULL) {
        //Rear Left wheel
        if (hod->rl != tic_rl) {
            if (tic_rl == 0) {
                dist_rl += 6.454;
                cout << dist_rl << endl;
            }
            tic_rl = hod->rl;
        }

        //Rear Right wheel
        if (hod->rr != tic_rr) {
            if (tic_rr == 0) {
                dist_rr += 6.454;
                cout << dist_rr << endl;
            }
            tic_rr = hod->rr;
        }

        //Front Left wheel
        if (hod->fl != tic_fl) {
            if (tic_fl == 0) {
                dist_fl += 6.454;
                cout << dist_fl << endl;
            }
            tic_fl = hod->fl;
        }

        //Front Right wheel
        if (hod->fr != tic_fr) {
            if (tic_fr == 0) {
                dist_fr += 6.454;
                cout << dist_fr << endl;
            }
            tic_fr = hod->fr;
        }
    }
}

void Core::virage(char sens) {
    int8_t left = 0;
    int8_t right = 0;
    if (sens == 'd' || sens == 'D') {
        left = 63;
        right = 10;
    } else if (sens == 'g' || sens == 'G') {
        left = 10;
        right = 63;
    }
    else {
        left = 0;
        right = 0;
        printf("Unknow command");
    }

    // COMMANDE MOTEUR
    last_motor_access_.lock();
    last_left_motor_ = static_cast<int8_t >( left * 2 );
    last_right_motor_ = static_cast<int8_t >( right * 2 );
    last_motor_access_.unlock();
}

void Core::deplacement(int direction) {
    int8_t left = 0;
    int8_t right = 0;

    left = 10 * direction * pid;
    right = 10 * direction * pid;

    // COMMANDE MOTEUR
    last_motor_access_.lock();
    last_left_motor_ = static_cast<int8_t >( left * 2 );
    last_right_motor_ = static_cast<int8_t >( right * 2 );
    last_motor_access_.unlock();
}
