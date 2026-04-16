#include "kernel/types.h"   // 定义基本类型，如 int、uint 等
#include "kernel/stat.h"    // 文件状态相关（很多程序都带着，习惯包含）
#include "user/user.h"      // 用户可用的系统调用声明，如 pipe/fork/read/write/getpid

int
main(){
	//定义两个数组来表示管道的两端'
	int p1[2],p2[2];// p1: 父→子，p2: 子→父

	pipe(p1);
	pipe(p2);

	//创建子进程
	int pid=fork();

	//是父进程先传给子进程，子进程再传给父进程
	if(pid==0){
		//子进程
		close(p1[1]);//子进程不需要写p1
		close(p2[0]);//子进程不需要读p2

		char buf[1];//存储从子进程发来的一个字节内容，所以是char类型
		//子进程读父进程
		read(p1[0],buf,1);
		printf("%d:received ping\n",getpid());
		write(p2[1],buf,1);

		close(p1[0]);
		close(p2[1]);
		exit(0);
	}else{
		//父进程
		close(p1[0]);
		close(p2[1]);

		char buf[1]={'x'};
		write(p1[1],buf,1);
		read(p2[0],buf,1);
		printf("%d:received pong\n",getpid());

		close(p1[1]);
		close(p2[0]);
		exit(0);
	}
}
