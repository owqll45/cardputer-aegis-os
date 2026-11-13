#include "system_manager.h"
#include <mooncake_log.h>
#include "../crash_handler.h"

static const std::string _tag = "System";

SystemManager::SystemManager()
{
}

SystemManager::~SystemManager()
{
}

void SystemManager::init()
{
    mclog::tagInfo(_tag, "system init");
}

void SystemManager::update()
{
}

void SystemManager::requestSystemPopup(const std::string& title, const std::string& msg, int type)
{
    _system_popup_active = true;

    /* Use the crash_handler to show popup */
    /* 0: Warning/Info, 1: Error */
    if (type == 1)
        crash_handler::show_system_popup(title.c_str(), msg.c_str(), crash_handler::POPUP_TYPE_ERROR, true);
    else
        crash_handler::show_system_popup(title.c_str(), msg.c_str(), crash_handler::POPUP_TYPE_WARNING, true);
}

void SystemManager::reportAppHealth()
{
    /* This will be handled by HalFacade to update timestamp */
}

void SystemManager::forceKillCurrentApp()
{
    /* Implement force kill logic */
}
