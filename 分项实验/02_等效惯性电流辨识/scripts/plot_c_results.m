function plot_c_results(protocolId, root)
arguments
    protocolId (1,1) string
    root (1,1) string = string(fileparts(fileparts(mfilename('fullpath'))))
end
assert(~isempty(regexp(protocolId,'^Z-BP_\d{8}-\d{6}_\d+_\d+$','once')));
prefix=fullfile(root,"results","c",protocolId);
out=fullfile(root,"data","figures","C",protocolId);
if ~isfolder(out), mkdir(out); end
s=jsondecode(fileread(prefix+"_summary.json"));
r=readtable(prefix+"_representative.csv",TextType="string");
assert(~isempty(r),"Missing representative trajectory.");
f=canvas([1000 680]);
t=tiledlayout(f,2,1,TileSpacing='compact',Padding='compact');
speed=nexttile(t);
electric=nexttile(t);
for phase=["acc","dec"]
    keep=r.phase==phase;
    plot(speed,r.card_ms(keep),r.speed_pulse_s(keep)/1000,Color=[0.08 0.19 0.29],LineWidth=1.25,HandleVisibility='off');
    plot(electric,r.card_ms(keep),r.corrected_mA(keep),Color=[0.08 0.19 0.29],LineWidth=1);
end
acc=[r.acc_begin_ms(1),r.acc_end_ms(1)];
dec=[r.dec_begin_ms(1),r.dec_end_ms(1)];
for a=[speed electric]
    shade(a,acc,[0.26 0.57 0.78],"加速积分");
    shade(a,dec,[0.85 0.47 0.29],"减速积分");
    xlim(a,[min(r.card_ms),max(r.card_ms)]);
end
yline(speed,12,':k',HandleVisibility='off');
yline(speed,24,':k',HandleVisibility='off');
ylabel(speed,mix('编码器拟合速度','mm/s'));
ylabel(electric,mix('位置修正电流','mA'));
xlabel(electric,mix('卡内相对时间','ms'));
legend(speed,'Location','best','FontName','SimSun','Box','off');
title(t,"代表性轨迹  ·  "+s.representative,FontName='SimSun',Interpreter='none');
savePlots(f,out,protocolId+"_01_integral_segments");

pairs=readtable(prefix+"_pairs.csv",TextType="string");
pairs=pairs(pairs.valid==1 & pairs.band=="main",:);
g=readtable(prefix+"_groups.csv",TextType="string");
g=g(g.band=="main",:);
tiers=sort(unique(pairs.acc))/1000;
speeds=sort(unique(pairs.speed));
f=canvas([1150 500]);
t=tiledlayout(f,1,2,TileSpacing='compact',Padding='compact');
colors=[0.06 0.32 0.57; 0.67 0.26 0.13];
for panel=1:2
    a=nexttile(t);
    if panel==1
        ticks=tiers(tiers>=400 & tiers<=600);
        title(a,"中段主分析",FontName='SimSun',FontWeight='normal');
    else
        ticks=tiers(tiers<400 | tiers>600);
        title(a,"区间外对照",FontName='SimSun',FontWeight='normal');
    end
    for i=1:numel(speeds)
        offset=(i-1.5)*11;
        for j=1:numel(ticks)
            tier=ticks(j);
            use=pairs.acc==tier*1000 & pairs.speed==speeds(i);
            y=pairs.alpha(use)*1e5;
            group=g(g.group=="tier_"+string(tier*1000)+"_speed_"+string(speeds(i)),:);
            if isempty(y) || isempty(group), continue; end
            jitter=linspace(-2,2,numel(y))';
            scatter(a,tier+offset+jitter,y,22,colors(i,:),'filled', ...
                MarkerFaceAlpha=0.4,HandleVisibility='off');
            if isfinite(group.ci_low)
                center=group.mean_alpha*1e5;
                errorbar(a,tier+offset,center,(group.mean_alpha-group.ci_low)*1e5, ...
                    (group.ci_high-group.mean_alpha)*1e5,'o',Color=colors(i,:), ...
                    MarkerFaceColor=colors(i,:),LineWidth=1.2,HandleVisibility='off');
            end
        end
        plot(a,NaN,NaN,'o',Color=colors(i,:),MarkerFaceColor=colors(i,:), ...
            DisplayName=sprintf('%g mm/s',speeds(i)/1000));
    end
    yline(a,s.snapshot.bAlpha*1e5,'--k',LineWidth=1.2,DisplayName="B 冻结系数");
    xlim(a,[min(ticks)-20,max(ticks)+20]); xticks(a,ticks);
    xlabel(a,mix('指令加速度','mm/s^{2}'));
    ylabel(a,mix('积分系数','10^{-5} raw/(pulse/s^{2})'));
    legend(a,Location='best',FontName='SimSun',Box='off');
end
savePlots(f,out,protocolId+"_02_coefficient_by_tier");
end

function shade(a,window,color,label)
lims=ylim(a);
p=patch(a,[window fliplr(window)],[lims(1) lims(1) lims(2) lims(2)],color, ...
    FaceAlpha=0.18,EdgeColor='none',DisplayName=label);
uistack(p,'bottom');
ylim(a,lims);
end

function f=canvas(dim)
f=figure(Visible='off',Color='w',Position=[100 100 dim],DefaultAxesFontName='Times New Roman', ...
    DefaultAxesFontSize=12,DefaultAxesBox='on',DefaultAxesYGrid='on', ...
    DefaultAxesGridAlpha=0.12,DefaultAxesNextPlot='add');
end

function label=mix(name,unit)
label=['\fontname{SimSun}' name '（\fontname{Times New Roman}' unit '\fontname{SimSun}）'];
end

function savePlots(f,out,name)
savefig(f,fullfile(out,name+".fig"));
exportgraphics(f,fullfile(out,name+".svg"),ContentType='vector');
exportgraphics(f,fullfile(out,name+".png"),Resolution=400);
close(f);
end
