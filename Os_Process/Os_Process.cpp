
#include <iostream>
#include <thread>
#include <mutex>
#include <Windows.h> //sleep()
#include <vector>
#include <queue>
#include <fstream> // 파일 입력을 위해 추가
#include <string>

using namespace std;
mutex mtx;

//process 상태
enum class ProcessState {
    READY,
    RUNNING,
    WAITING
};

//프로세스 종류
enum class ProcessType {
    FOREGROUND,
    BACKGROUND
};

//Process 구조체
struct Process {
    int pid;
    ProcessType type;
    ProcessState state;
    int remainingTime; //WAITING 상태일 때 남은 시간
};

//스택 노드 구조체
struct StackNode {
    vector<Process*> processes;
    StackNode* next;
};

//Dynamic Queue
class DynamicQueue {
public:
    StackNode* top;
    StackNode* bottom;
    StackNode* P;
    int threshold;

    DynamicQueue() : top(nullptr), bottom(nullptr), P(nullptr), threshold(0) {}

    //enqueue()

    //dequeue()

    //promote()

    //split_n_merge()
};

//Wait Queue
queue<Process*> waitQueue;

//mtx 조건변수
condition_variable cv;

// 프로세스 목록 (shell, monitor)
vector<Process*> processes;

// 스케줄링 함수
void scheduler() {
    while (true) {
        // 1초 대기
        this_thread::sleep_for(chrono::seconds(1));

        // 뮤텍스 잠금
        unique_lock<mutex> lock(mtx);

        // Dynamic Queue에서 프로세스 실행 및 관리
        // ... (dequeue(), promote(), split_n_merge() 호출)

        // Wait Queue에서 프로세스 상태 업데이트
        // ... (대기 시간 감소, READY 상태로 변경)

        // 뮤텍스 잠금 해제
        lock.unlock();

        // 조건 변수 알림
        cv.notify_all();
    }
}

// shell 프로세스 함수
void shellProcess(int pid, int delay) {
    ifstream inputFile("input.txt"); // 파일 입력

    while (inputFile) {
        // 파일에서 명령어 읽기
        string command;
        getline(inputFile, command);

        // 명령어 처리
        // ... (실제 명령어 처리 로직 구현)

        // delay 시간 동안 대기
        this_thread::sleep_for(chrono::milliseconds(delay));
    }
}

// monitor 프로세스 함수
void monitorProcess(int pid, int delay) {
    while (true) {
        // 뮤텍스 잠금
        unique_lock<mutex> lock(mtx);

        // 시스템 상태 출력
        // ... (Running, DQ, WQ 상태 출력)

        // 뮤텍스 잠금 해제
        lock.unlock();

        // delay 시간 동안 대기
        this_thread::sleep_for(chrono::milliseconds(delay));
    }
}



int main()
{
    // 프로세스 생성 (shell, monitor)
    Process shell = { 0, ProcessType::FOREGROUND, ProcessState::READY, 0 };
    Process monitor = { 1, ProcessType::BACKGROUND, ProcessState::READY, 0 };
    processes.push_back(&shell);
    processes.push_back(&monitor);

    // 스레드 생성 (shell, monitor, scheduler)
    thread t1(shellProcess, shell.pid, 1500); // shell 프로세스, 1.5초 간격
    thread t2(monitorProcess, monitor.pid, 1000); // monitor 프로세스, 1초 간격
    thread t3(scheduler); // 스케줄러 스레드

    t1.join();
    t2.join();
    t3.join(); // 실제로는 scheduler는 무한 루프이므로 join하지 않아야 함

    return 0;
}

