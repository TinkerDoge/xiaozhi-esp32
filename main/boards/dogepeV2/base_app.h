#pragma once

class BaseApp {
public:
    virtual ~BaseApp() = default;
    
    virtual void OnStart() = 0;
    virtual void OnStop() = 0;
    
    virtual void OnButtonAClick() {}
    virtual void OnButtonALongPress() {}
    virtual void OnButtonBClick() {}
};
