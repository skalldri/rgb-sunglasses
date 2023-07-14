#pragma once

template<class T>
class Singleton {
    public:
        static T& getInstance()
        {
            static T inst;
            return inst;
        }
    
    protected:
        Singleton() = default; // Make constructors private :D
};