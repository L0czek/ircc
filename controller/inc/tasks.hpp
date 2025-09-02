#ifndef __TASKS_HPP__
#define __TASKS_HPP__

class Task {
public:
    Task(const Task& ) = delete;
    Task& operator=(const Task& ) = delete;

    virtual void run() =0;
};

class IdleTask :private Task {
    
};

#endif

