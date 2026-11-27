function plot_quality
folder=fileparts(mfilename('fullpath'));
c=readtable(fullfile(folder,'conditions.csv'),TextType='string');
t=readtable(fullfile(folder,'legs.csv'),TextType='string');
t=t(t.status=="valid",:);
f=figure(Visible='off',Color='w',Position=[40 40 1170 680]);
tiledlayout(f,2,2,TileSpacing='compact',Padding='compact');
for p=["P1","P2"]
    z=c(c.path==p,:);
    a=nexttile; bar(a,[z.pos_joint z.neg_joint]); grid(a,'on');
    xticks(a,1:height(z));xticklabels(a,compose('%g/%g',z.v,z.a));xtickangle(a,65);
    xlabel(a,'速度 / 加速度（mm/s，mm/s²）');ylabel(a,'联合匹配格点数');
    title(a,p+" 路径的状态覆盖");legend(a,{'正向','反向'},Location='northwest');
end
a=nexttile; hold(a,'on');grid(a,'on');
for p=["P1","P2"]
    z=c(c.path==p,:);
    plot(a,z.v,1.2*z.pos_shift_raw,'o',DisplayName=p+" 正向");
    plot(a,z.v,1.2*z.neg_shift_raw,'+',DisplayName=p+" 反向");
end
xlabel(a,'工况速度（mm/s）');ylabel(a,'两段稳态平均电流差（mA）');title(a,'同工况时段差 S2 − S1');legend(a,Location='best',NumColumns=2);
a=nexttile;histogram(a,t.host_gap_max_ms);grid(a,'on');xlabel(a,'每腿最大主机轮询间隔（ms）');ylabel(a,'测量腿数');title(a,'运动段主机轮询连续性');
set(findall(f,Type='axes'),FontName='SimSun',TickDir='in',Box='on',FontSize=10);
savefig(f,fullfile(folder,'quality.fig'));exportgraphics(f,fullfile(folder,'quality.png'),Resolution=400);close(f);
end
