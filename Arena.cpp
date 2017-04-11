#include <iostream>
#include <iomanip>
#include <sstream>
#include <fstream>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <poll.h>
#include <array>
#include <random>
#include <chrono>
#include <omp.h>
#include <limits>
#include <algorithm>
#include <map>
#include <thread>
#include <csignal>
using namespace std;
using namespace std::chrono;

constexpr bool Debug_AI{false},Timeout{true};
constexpr int PIPE_READ{0},PIPE_WRITE{1};
constexpr int N{2};//Number of players, 1v1
constexpr double FirstTurnTime{1*(Timeout?10:1)},TimeLimit{0.05*(Timeout?10:1)};
constexpr int extra_space_between_factories{300};
constexpr int W{16000},H{6500};
constexpr int Min_Production_Rate{4};
const array<string,3> Move_Type_String{"MOVE","BOMB","INC"};

bool stop{false};//Global flag to stop all arena threads when SIGTERM is received

struct vec{
	int x,y;
	inline vec operator-(const vec &a)noexcept{
		return vec{x-a.x,y-a.y};
	}
};

struct factory{
    int owner,units,prod,turns;
    vec r;
    vector<int> L;
};

struct troop{
    int owner,source,target,units,turns;
};

struct bomb{
    int owner,source,target,turns;
};

struct state{
    vector<factory> F;
    vector<troop> T;
    map<int,bomb> B;
    array<int,2> N_Bombs;
    int entityId;
};

enum move_type{MOVE=0,BOMB=1,INCREASE=2};

struct play{
    move_type type;
    int from,to,amount;
};

typedef vector<play> strat;

ostream& operator<<(ostream &os,const play &m){
    os << Move_Type_String[m.type] << " " << m.from << " " << m.to << " " << m.amount;
    return os;
}

ostream& operator<<(ostream &os,const vec &r){
	os << r.x << " " << r.y;
	return os;
}

inline string EmptyPipe(const int fd){
	int nbytes;
	if(ioctl(fd,FIONREAD,&nbytes)<0){
		throw(4);
	}
	string out;
	out.resize(nbytes);
	if(read(fd,&out[0],nbytes)<0){
		throw(4);
	}
	return out;
}

struct AI{
	int id,pid,outPipe,errPipe,inPipe;
	string name;
	inline void stop(){
		if(alive()){
			kill(pid,SIGTERM);
			int status;
			waitpid(pid,&status,0);//It is necessary to read the exit code for the process to stop
			if(!WIFEXITED(status)){//If not exited normally try to "kill -9" the process
				kill(pid,SIGKILL);
			}
		}
	}
	inline bool alive()const{
		return kill(pid,0)!=-1;//Check if process is still running
	}
	inline void Feed_Inputs(const string &inputs){
		if(write(inPipe,&inputs[0],inputs.size())==-1){
			throw(5);
		}
	}
	inline ~AI(){
		close(errPipe);
		close(outPipe);
		close(inPipe);
		stop();
	}
};

void StartProcess(AI &Bot){
	int StdinPipe[2];
  	int StdoutPipe[2];
  	int StderrPipe[2];
  	if(pipe(StdinPipe)<0){
	    perror("allocating pipe for child input redirect");
  	}
  	if(pipe(StdoutPipe)<0){
	    close(StdinPipe[PIPE_READ]);
	    close(StdinPipe[PIPE_WRITE]);
	    perror("allocating pipe for child output redirect");
  	}
  	if(pipe(StderrPipe)<0){
  		close(StderrPipe[PIPE_READ]);
  		close(StderrPipe[PIPE_WRITE]);
  		perror("allocating pipe for child stderr redirect failed");
  	}
  	int nchild{fork()};
  	if(nchild==0){//Child process
	    if(dup2(StdinPipe[PIPE_READ],STDIN_FILENO)==-1){// redirect stdin
			perror("redirecting stdin");
			return;
	    }
	    if(dup2(StdoutPipe[PIPE_WRITE],STDOUT_FILENO)==-1){// redirect stdout
			perror("redirecting stdout");
			return;
	    }
	    if(dup2(StderrPipe[PIPE_WRITE],STDERR_FILENO)==-1){// redirect stderr
			perror("redirecting stderr");
			return;
	    }
	    close(StdinPipe[PIPE_READ]);
	    close(StdinPipe[PIPE_WRITE]);
	    close(StdoutPipe[PIPE_READ]);
	    close(StdoutPipe[PIPE_WRITE]);
	    close(StderrPipe[PIPE_READ]);
	    close(StderrPipe[PIPE_WRITE]);
	    execl(Bot.name.c_str(),Bot.name.c_str(),(char*)NULL);//(char*)Null is really important
	    //If you get past the previous line its an error
	    perror("exec of the child process");
  	}
  	else if(nchild>0){//Parent process
  		close(StdinPipe[PIPE_READ]);//Parent does not read from stdin of child
    	close(StdoutPipe[PIPE_WRITE]);//Parent does not write to stdout of child
    	close(StderrPipe[PIPE_WRITE]);//Parent does not write to stderr of child
    	Bot.inPipe=StdinPipe[PIPE_WRITE];
		Bot.outPipe=StdoutPipe[PIPE_READ];
    	Bot.errPipe=StderrPipe[PIPE_READ];
    	Bot.pid=nchild;
  	}
  	else{//failed to create child
  		close(StdinPipe[PIPE_READ]);
	    close(StdinPipe[PIPE_WRITE]);
	    close(StdoutPipe[PIPE_READ]);
	    close(StdoutPipe[PIPE_WRITE]);
	    perror("Failed to create child process");
  	}
}

inline bool Invalid_Factory_Id(const state &S,const int id)noexcept{
	return id<0 || id>=S.F.size();
}

void Simulate_Player_Action(state &S,const strat &Moves,const int color){
	vector<int> Sent(pow(S.F.size(),2),0);
    for(const play &m:Moves){
    	if(Invalid_Factory_Id(S,m.from)){
    		cerr << "INVALID: " << m << endl;
    		throw(2);
    	}
    	if(S.F[m.from].owner!=color){
    		throw(2);
    	}
        factory &f{S.F[m.from]};
        if(m.type==MOVE){
        	if(m.from==m.to || m.amount<0 || Invalid_Factory_Id(S,m.to)){
        		throw(2);
        	}
        	int real_dispatchment{min(m.amount,f.units)};
        	if(real_dispatchment>0){
				f.units-=real_dispatchment;
				Sent[m.from*S.F.size()+m.to]+=real_dispatchment;
        	} 
        }
        else if(m.type==BOMB){
        	if(m.from==m.to || Invalid_Factory_Id(S,m.to)){
        		throw(2);
        	}
        	if(S.N_Bombs[color==1?0:1]!=0){
        		--S.N_Bombs[color==1?0:1];
            	S.B[S.entityId++]=bomb{color,m.from,m.to,f.L[m.to]+1};
        	}
        }
        else{//Increase
        	if(f.units>=10 && f.prod<3){
        		f.units-=10;
            	++f.prod;
        	}
        }
    }
    for(int i=0;i<S.F.size();++i){
    	for(int j=0;j<S.F.size();++j){
    		if(i!=j && Sent[i*S.F.size()+j]>0){
    			S.T.push_back(troop{color,i,j,Sent[i*S.F.size()+j],S.F[i].L[j]+1});//+1 distance to compensate for the fact that I do player moves before decreasing turn counters
    		}
    	}
    }
}

void Simulate(state &S){
    vector<int> Arrived_Troops(S.F.size(),0);
    for(auto it=S.T.begin();it!=S.T.end();){//Troop movement
        troop &t{*it};
        --t.turns;
        if(t.turns==0){
            Arrived_Troops[t.target]+=t.units*t.owner;//Units that arrive to a factory on this turn fight first
            it=S.T.erase(it);
        }
        else{
            ++it;
        }
    }
    for(factory &f:S.F){//Production
        if(f.owner!=0){
            f.turns=max(0,f.turns-1);
            if(f.turns==0){
                f.units+=f.prod;  
            }
        }
    }
    for(int i=0;i<S.F.size();++i){//Battles
        if(Arrived_Troops[i]!=0 && Arrived_Troops[i]*S.F[i].owner<=0/*Different sign->fight*/){
            if(abs(Arrived_Troops[i])>S.F[i].units){
                S.F[i].owner=Arrived_Troops[i]>0?1:-1;
                S.F[i].units=abs(Arrived_Troops[i])-S.F[i].units;
            }
            else{
                S.F[i].units-=abs(Arrived_Troops[i]);
            }
        }
        else{
            S.F[i].units+=abs(Arrived_Troops[i]);
        }
    }
    for(auto it=S.B.begin();it!=S.B.end();){//Bombs move and explode
        bomb &b{it->second};
        --b.turns;
        if(b.turns==0){
            factory &f{S.F[b.target]};
            f.units-=max(min(10,f.units),f.units/2);
            f.turns=5;
            it=S.B.erase(it);
        }
        else{
            ++it;
        }
    }
}

void Make_Move(state &S,AI &Bot,const string &Move){
	//cerr << Move << endl;
	stringstream ss(Move);
	string type;
	ss >> type;
	if(type=="WAIT"){
		return;
	}
	else{
		strat M;
		size_t delim{0};
		while(delim!=string::npos){
			stringstream ss(Move.substr(delim,Move.find_first_of(';',delim)));
			delim=Move.find_first_of(';',delim);
			if(delim!=string::npos){
				++delim;
			}
			play m;
			string type;
			ss >> type;
			if(type=="MOVE"){
				m.type=MOVE;
				ss >> m.from >> m.to >> m.amount;
			}
			else if(type=="BOMB"){
				m.type=BOMB;
				ss >> m.from >> m.to;
			}
			else if(type=="INC"){
				m.type=INCREASE;
				ss >> m.from;
			}
			else if(type=="MSG"){
				continue;
			}
			else{
				cerr << "Unrecognised move from AI " << Bot.id << " name: " << Bot.name << ": " << Move << endl;
				throw(3);
			}
			M.push_back(m);
		}
		Simulate_Player_Action(S,M,Bot.id==1?-1:1);
	}
}

string GetMove(AI &Bot,const int turn){
	pollfd outpoll{Bot.outPipe,POLLIN};
	time_point<system_clock> Start_Time{system_clock::now()};
	while(static_cast<duration<double>>(system_clock::now()-Start_Time).count()<(turn==1?FirstTurnTime:TimeLimit)){
		double TimeLeft{(turn==1?FirstTurnTime:TimeLimit)-static_cast<duration<double>>(system_clock::now()-Start_Time).count()};
		if(poll(&outpoll,1,TimeLeft)){
			return EmptyPipe(Bot.outPipe);
		}
	}
	throw(1);
}

inline bool Has_Won(const array<AI,N> &Bot,const int idx)noexcept{
	for(int i=0;i<N;++i){
		if(i!=idx && Bot[i].alive()){
			return false;
		}
	}
	return true;
}

inline void Play_Move(state &S,AI &Bot,const string &M){
	try{
		Make_Move(S,Bot,M);
		string err_str{EmptyPipe(Bot.errPipe)};
		if(Debug_AI){
			ofstream err_out("log.txt",ios::app);
			err_out << err_str << endl;
		}
	}
	catch(const int ex){
		if(ex==2){
			cerr << "Invalid move from AI " << Bot.id << " name: " << Bot.name << endl;
		}
		else if(ex==3){
			//cerr << "Unrecognised move from AI " << Bot.id << " name: " << Bot.name << endl;
		}
		else if(ex==4){
			cerr << "Error emptying buffer from AI " << Bot.id << " name: " << Bot.name << endl;
		}
		else if(ex==5){
			cerr << "Error writing to AI " << Bot.id << " name: " << Bot.name << endl; 
		}
		Bot.stop();
	}
}

inline bool Player_Alive(const state &S,const int color)noexcept{
	return find_if(S.F.begin(),S.F.end(),[&](const factory &f){return f.owner==color && (f.units!=0 || f.prod!=0);})!=S.F.end() || find_if(S.T.begin(),S.T.end(),[&](const troop &t){return t.owner==color;})!=S.T.end();
}

inline bool All_Dead(const array<AI,N> &Bot)noexcept{
	for(const AI &b:Bot){
		if(b.alive()){
			return false;
		}
	}
	return true;
}

int Play_Game(const array<string,N> &Bot_Names,state &S){
	array<AI,N> Bot;
	for(int i=0;i<N;++i){
		Bot[i].id=i;
		Bot[i].name=Bot_Names[i];
		StartProcess(Bot[i]);
		stringstream ss;
		ss << S.F.size() << " " << S.F.size()*(S.F.size()-1)/2 << endl;
		for(int j=0;j<S.F.size();++j){
			for(int k=j+1;k<S.F.size();++k){
				ss << j << " " << k << " " << S.F[j].L[k] << endl;
			}
		}
		Bot[i].Feed_Inputs(ss.str());
	}
	S.entityId=0;
	int turn{0};
	while(++turn>0 && !stop){
		array<string,2> M{"WAIT","WAIT"};
		for(int i=0;i<N;++i){
			if(Bot[i].alive()){
				if(Has_Won(Bot,i)){
					//cerr << i << " has won in " << turn << " turns" << endl;
					return i;
				}
				int color{i==0?1:-1};
				stringstream ss;
				ss << S.F.size()+S.T.size()+S.B.size() << endl;
				for(int j=0;j<S.F.size();++j){
					ss << j << " FACTORY " << color*S.F[j].owner << " " << S.F[j].units << " " << S.F[j].prod << " " << S.F[j].turns << " " << 0 << endl;
				}
				for(int j=0;j<S.T.size();++j){
					ss << j << " TROOP " << color*S.T[j].owner << " " << S.T[j].source << " " << S.T[j].target << " " << S.T[j].units << " " << S.T[j].turns << endl;
				}
				for(const auto &it:S.B){
					const bomb &b{it.second};
					ss << it.first << " BOMB " << color*b.owner << " " << b.source << " " << (b.owner==color?b.target:-1) << " " << (b.owner==color?b.turns:-1) << " " << 0 << endl;
				}
				Bot[i].Feed_Inputs(ss.str());
				try{
					M[i]=GetMove(Bot[i],turn);
					//cerr << M[i] << endl;
				}
				catch(int ex){
					if(ex==1){//Timeout
						cerr << "Loss by Timeout of AI " << Bot[i].id << " name: " << Bot[i].name << endl;
					}
					Bot[i].stop();
				}
			}
		}
		for(int i=0;i<2;++i){
			Play_Move(S,Bot[i],M[i]);
		}
		Simulate(S);
		for(int i=0;i<N;++i){
			if(!Player_Alive(S,i==0?1:-1)){
				Bot[i].stop();
			}
		}
		if(turn==200){
			array<int,2> Units;
			fill(Units.begin(),Units.end(),0);
			for(const factory &f:S.F){
				if(f.owner==1){
					Units[0]+=f.units;
				}
				else if(f.owner==-1){
					Units[1]+=f.units;
				}
			}
			for(const troop &t:S.T){
				if(t.owner==1){
					Units[0]+=t.units;
				}
				else if(t.owner==-1){
					Units[1]+=t.units;
				}
			}
			if(Units[0]==Units[1]){
				//cerr << "Draw" << endl;
				return -1;
			}
			else if(Units[0]>Units[1]){
				//cerr << 0 << " has won in " << turn << " turns" << endl;
				return 0;
			}
			else{
				//cerr << 1 << " has won in " << turn << " turns" << endl;
				return 1;
			}
		}
		else if(All_Dead(Bot)){
			return -1;
		}
	}
	return -2;
}

inline double Distance(const vec &a,const vec &b)noexcept{
	return sqrt(pow(a.x-b.x,2)+pow(a.y-b.y,2));
}

inline bool Valid_Spawn(const vec &r,const state &S,const int id,const int minSpaceBetweenFactories)noexcept{
	for(int j=0;j<id;++j){
		if(Distance(r,S.F[j].r)<minSpaceBetweenFactories){
			return false;
		}
	}
	return true;
}

void Output_Stats(const array<string,N> &Bot_Names,const int winner){
	string stats_filename{Bot_Names[0]+"_vs_"+Bot_Names[1]+"_Stats.txt"};
	ofstream stats_file(stats_filename,ios::app);
	stats_file << winner << endl;
}

int Play_Round(array<string,N> Bot_Names){
	default_random_engine generator(system_clock::now().time_since_epoch().count());
	uniform_int_distribution<int> Swap_Distrib(0,1);
	const bool player_swap{Swap_Distrib(generator)==1};
	if(player_swap){
		swap(Bot_Names[0],Bot_Names[1]);
	}
	state S;
	uniform_int_distribution<int> Factories_Distrib(7,15);
	S.F.resize(Factories_Distrib(generator));
	if(S.F.size()%2==0){//factoryCount must be odd
		S.F.resize(S.F.size()+1);
	}
	int factory_radius{S.F.size()>10?600:700};
	int minSpaceBetweenFactories{2*(factory_radius+extra_space_between_factories)};
	S.F[0]=factory{0,0,0,0,{W/2,H/2},{}};
	uniform_int_distribution<int> X_Distrib(0,W/2-2*factory_radius),Y_Distrib(0,H-2*factory_radius);
	uniform_int_distribution<int> Production_Distrib(0,3),Initial_Units_Distrib(15,30);
	for(int i=1;i<S.F.size();i+=2){
		vec r;
		do{
			r=vec{X_Distrib(generator)+factory_radius+extra_space_between_factories,Y_Distrib(generator)+factory_radius+extra_space_between_factories};
		}while(!Valid_Spawn(r,S,i,minSpaceBetweenFactories));
		int prod{Production_Distrib(generator)};
		if(i==1){
			int units{Initial_Units_Distrib(generator)};
			S.F[i]=factory{1,units,prod,0,r,{}};
			S.F[i+1]=factory{-1,units,prod,0,vec{W,H}-r,{}};
		}
		else{
			uniform_int_distribution<int> Initial_Neutrals_Distrib(0,5*prod);
			int units{Initial_Neutrals_Distrib(generator)};
			S.F[i]=factory{0,units,prod,0,r,{}};
			S.F[i+1]=factory{0,units,prod,0,vec{W,H}-r,{}};
		}
	}
	int total_prod{0};
	for(int i=0;i<S.F.size();++i){
		total_prod+=S.F[i].prod;
		S.F[i].L.resize(S.F.size());
	}
	for(int i=0;i<S.F.size();++i){
		for(int j=i+1;j<S.F.size();++j){
			int d{static_cast<int>(round((Distance(S.F[i].r,S.F[j].r)-2*factory_radius)/800.0))};
			S.F[i].L[j]=d;
			S.F[j].L[i]=d;
		}
	}
	for(int i=1;total_prod<Min_Production_Rate && i<S.F.size();++i){
		factory &f{S.F[i]};
		if(f.prod<3){
			++f.prod;
			++total_prod;
		}
	}
	fill(S.N_Bombs.begin(),S.N_Bombs.end(),2);
	int winner{Play_Game(Bot_Names,S)};
	if(player_swap){
		return winner==-1?-1:winner==0?1:0;
	}
	else{
		return winner;
	}
}

void StopArena(const int signum){
	stop=true;
}

int main(int argc,char **argv){
	if(argc<3){
		cerr << "Program takes 2 inputs, the names of the AIs fighting each other" << endl;
		return 0;
	}
	int N_Threads{1};
	if(argc>=4){//Optional N_Threads parameter
		N_Threads=min(2*omp_get_num_procs(),max(1,atoi(argv[3])));
		cerr << "Running " << N_Threads << " arena threads" << endl;
	}
	array<string,N> Bot_Names;
	for(int i=0;i<2;++i){
		Bot_Names[i]=argv[i+1];
	}
	cout << "Testing AI " << Bot_Names[0];
	for(int i=1;i<N;++i){
		cerr << " vs " << Bot_Names[i];
	}
	cerr << endl;
	for(int i=0;i<N;++i){//Check that AI binaries are present
		ifstream Test{Bot_Names[i].c_str()};
		if(!Test){
			cerr << Bot_Names[i] << " couldn't be found" << endl;
			return 0;
		}
		Test.close();
	}
	signal(SIGTERM,StopArena);//Register SIGTERM signal handler so the arena can cleanup when you kill it
	int games{0},draws{0};
	array<double,2> points{0,0};
	#pragma omp parallel num_threads(N_Threads) shared(games,points,Bot_Names)
	while(!stop){
		int winner{Play_Round(Bot_Names)};
		if(winner==-1){//Draw
			#pragma omp atomic
			++draws;
			#pragma omp atomic
			points[0]+=0.5;
			#pragma omp atomic
			points[1]+=0.5;
		}
		else{//Win
			++points[winner];
		}
		#pragma omp atomic
		++games;
		double p{static_cast<double>(points[0])/games};
		double sigma{sqrt(p*(1-p)/games)};
		double better{0.5+0.5*erf((p-0.5)/(sqrt(2)*sigma))};
		#pragma omp critical
		cout << "Wins:" << setprecision(4) << 100*p << "+-" << 100*sigma << "% Rounds:" << games << " Draws:" << draws << " " << better*100 << "% chance that " << Bot_Names[0] << " is better" << endl;
	}
}