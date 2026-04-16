#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int
main(int argc,char* argv[]){
	//步骤1：检查参数个数，如果没有提供参数就提醒用户
	if(argc<2){
		fprintf(2,"Usage:sleep <ticks>\n");//2是报错文件标志符
		exit(0);
	}

	//步骤2：将字符串参数转换为整数
	//argv[0]是程序名"sleep"，argv[1]才是用户输入的数字
	int ticks=atoi(argv[1]);

	//步骤3：添加sleep系统调用
	sleep(ticks);

	//步骤4：退出程序
	exit(0);
}
