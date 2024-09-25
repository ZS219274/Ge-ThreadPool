#include "threadpool.h"



const int TASK_MAX_THREADHOLD = INT32_MAX;
const int THREAD_MAX_THREADHOLD = 10;
const int THREAD_MAX_IDEL_TIME = 10;

// 线程池构造
ThreadPool::ThreadPool() :
    initThreadSize_(std::thread::hardware_concurrency()),
    taskSize_(0),
    taskQueMaxThreadHold_(TASK_MAX_THREADHOLD),
    poolMode_(PoolMode::MODE_FIXED),
    isPoolRunning_(false),
    idleThreadSize_(0),
    threadSizeThreadhold_(THREAD_MAX_THREADHOLD),
    curThreadSize_(0)
{}

// 线程池析构
ThreadPool::~ThreadPool()
{
    isPoolRunning_ = false;   //线程池要析构
     notEmpty_.notify_all();   //唤醒线程池中睡眠线程

    //等待线程池里所有线程返回，有两种状态 阻塞&执行任务
    std::unique_lock<std::mutex>lock(taskQueMtx_);       //负责析构的线程获取锁，被唤醒的线程则阻塞

   
    exitCond_.wait(lock, [&]()->bool {return threads_.size() == 0; }); //threads_.size()   //线程完成任务前阻塞析构线程

}

////检查pool的运行状态
bool ThreadPool::checkRunningState() const
{
    return isPoolRunning_;
}


// 开启线程池的工作模式
void ThreadPool::setMode(PoolMode mode)
{
    if (checkRunningState())
        return;
    poolMode_ = mode;
}

//设置线程池Cached模式下线程数量阈值
void ThreadPool::setThreadSizeThreadHold(int threadhold)
{
    if (checkRunningState())
        return;
    if (poolMode_ == PoolMode::MODE_CACHED) {
        threadSizeThreadhold_ = threadhold;
    }
}

// 设置task任务队列上限阈值
void ThreadPool::setTaskQueMaxThreadHold(int threadhold)
{
    if (checkRunningState())
        return;
    taskQueMaxThreadHold_ = threadhold;
}

// 生产者
//  给线程池提交任务对象,用户调用接口，传入任务对象，生产任务
Result ThreadPool::submitTask(std::shared_ptr<Task> sp) // 智能指针指向由抽象基类派生出的用户任务对象
{
    // 获取锁
    std::unique_lock<std::mutex> lock(taskQueMtx_);
    // 线程通信 等待任务队列有空余
    // 用户提交任务，最长不能阻塞超过1s，否则判断提交失败，返回
    if (!notFull_.wait_for(lock, std::chrono::seconds(1),
        [&]() -> bool { return taskQue_.size() < (size_t)taskQueMaxThreadHold_; }))
    {
        std::cerr << "task queue dull submit task fail." << std::endl;
        return Result(sp, false);
    }
    // 如果有空余，把任务放入任务队列中
    taskQue_.emplace(sp);
    taskSize_++;

    // 因为新放了任务，任务队列不空，在notEmpty_进行通知,赶快分配线程执行任务
    notEmpty_.notify_all();

    //Cached模式，任务处理比较紧急，场景：小而快的任务
    if (poolMode_ == PoolMode::MODE_CACHED
        && taskSize_ > idleThreadSize_
        && curThreadSize_ < threadSizeThreadhold_)
    {
        std::cout << ">>>>create new thread" << std::endl;

        //创建新线程
        auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));
        int threadId = ptr->getId();
        threads_.emplace(threadId, std::move(ptr));
        //启动线程
        threads_[threadId]->start();

        //修改线程相关的变量
        idleThreadSize_++;
        curThreadSize_++;
    }

    return Result(sp);
}

// 开启线程池
void ThreadPool::start(int initThreadSize)
{

    //运行后修改线程池状态变量
    isPoolRunning_ = true;
    // 记录初始线程数量
    initThreadSize_ = initThreadSize;
    curThreadSize_ = initThreadSize;

    // 创建线程对象
    for (int i = 0; i < initThreadSize_; i++)
    {
        // 创建线程对象的时候，把线程函数给到thread对象,通过绑定器将threadpool的线程函数绑定到线程
        auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));
        int threadId = ptr->getId();
        threads_.emplace(threadId, std::move(ptr));
        //threads_.emplace_back(std::move(ptr));

    }

    // 启动所有线程
    for (int i = 0; i < initThreadSize_; i++)
    {
        threads_[i]->start(); // 需要去执行一个线程函数
        idleThreadSize_++;   //记录初始空闲线程数量
    }
}

// 定义线程函数  线程池的所有线程从任务队列里消费任务
void ThreadPool::threadFunc(int threadid)
{
    auto lastTime = std::chrono::high_resolution_clock().now();
    for(;;)
    {
        std::shared_ptr<Task> task;
        {
            // 获取锁
            std::unique_lock<std::mutex> lock(taskQueMtx_);
            std::cout << "tid:" << std::this_thread::get_id() << "尝试获取任务" << std::endl;
            //cached模式下，可能创建了许多线程，但空闲事件超过60s,应把空闲线程回收
            //结束回收（超过initThreadSize_的数量的线程进行回收）
            //当前时间-上一次线程执行的时间 > 60s

            //每秒钟返回一次 怎么区分：超时返回？还是有任务待执行返回
            while (taskQue_.size() == 0)  //isPoolRunning_&&
            {
                //线程池要回收资源
                if (!isPoolRunning_)
                {
                    threads_.erase(threadid);
                    exitCond_.notify_all();
                    std::cout << "threadid:" << threadid << "exit" << std::endl;
                    return;
                }

                if (poolMode_ == PoolMode::MODE_CACHED)
                {
                    //条件变量超时返回
                    if (std::cv_status::timeout ==
                        notEmpty_.wait_for(lock, std::chrono::seconds(1)))
                    {
                        auto now = std::chrono::high_resolution_clock().now();
                        auto dur = std::chrono::duration_cast<std::chrono::seconds>(now - lastTime);
                        if (dur.count() >= THREAD_MAX_IDEL_TIME
                            && curThreadSize_ > initThreadSize_)
                        {
                            //开始回收当前线程
                            //记录线程相关变量的值修改
                            //把线程对象从线程列表容器中删除 没有办法匹配threadFunc和thread对象
                            //threadid =》thread对象=》删除
                            threads_.erase(threadid);
                            curThreadSize_--;
                            idleThreadSize_--;
                            std::cout << "threadid" << std::this_thread::get_id() << "exit" << std::endl;
                            return;
                        }
                    }
                }
                else
                {
                    //等待notEmpty条件
                    notEmpty_.wait(lock);
                }
                //休眠线程被唤醒发现线程池要回收资源
                // if (!isPoolRunning_)
                // {
                //     threads_.erase(threadid);
                //     std::cout << "休眠threadid" << std::this_thread::get_id() << "exit" << std::endl;
                //     exitCond_.notify_all();
                //     return;
                // }
            }

            

            idleThreadSize_--;
            std::cout << "tid:" << std::this_thread::get_id() << "获取任务成功" << std::endl;

            task = taskQue_.front(); // 任务队列中取一个任务出来
            taskQue_.pop();
            taskSize_--;

            if (taskQue_.size() > 0) // 如果依然有剩余任务，继续通知其他线程执行任务
            {
                notEmpty_.notify_all();
            }
            notFull_.notify_all(); // 取出一个任务，进行通知，通知可以继续进行生产任务
        }
        // 此处应释放锁
        if (task != nullptr)
        {
            //task->run(); 将run()操作封装进exec，在exec中发生多态，并且获得run的返回值
            task->exec();// 当前线程负责执行这个任务
        }
        lastTime = std::chrono::high_resolution_clock().now(); //更新线程执行完任务调度的时间
        idleThreadSize_++;
    }
}

// thread构造
Thread::Thread(ThreadFunc func)
    :func_(func),
    threadId_(generateId_++)
{
}

int Thread::generateId_ = 0;

// thread析构
Thread::~Thread()
{}

///////线程方法实现
// 启动线程
void Thread::start()
{
    // 创建一个线程来执行一个线程函数
    std::thread t(func_, threadId_); // c++11来说，线程对象t和线程函数绑定在一起，出了作用域{}，线程函数还是要继续执行
    t.detach();           // 设置线程分离
}

int Thread::getId()const
{
    return threadId_;
}



//Result构造函数
Result::Result(std::shared_ptr<Task> task, bool isValid)
    :task_(std::move(task)),
    isValid_(isValid)
{
    task_->setResult(this);
}


void Task::setResult(Result* res)
{
    result_ = res;
}

Task::Task()
    :result_(nullptr) {}

void Task::exec()
{
    if (result_ != nullptr)
    {
        result_->setVal(run()); //这里发生多态
    }
}

void Result::setVal(Any any) //
{
    //存储task的返回值
    this->any_ = std::move(any);
    sem_.post(); //已经获取任务的返回值，增加信号资源
}

Any Result::get() //用户调用
{
    if (!isValid_)
    {
        return "";
    }
    sem_.wait();  //task如果没有执行完，这里会阻塞用户的线程
    return std::move(any_);
}