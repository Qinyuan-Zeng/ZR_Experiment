function plot_position_report()
% Position report figures. Inputs are the archived A1/A2 windows, BC
% references and CV scores, frozen B nodes, and D paired/direction results.
% Current is converted from a33 feedback units to mA at 1.2 mA per count.
% Outputs: data/figures/position_report/position_01..08 as FIG, SVG and PNG.
root=fileparts(fileparts(mfilename('fullpath')));
results=fullfile(root,'results');
out=fullfile(root,'data','figures','position_report');
if ~isfolder(out), mkdir(out); end
bc=@(suffix) read(fullfile(results,'BC_position',"Z-BC_20261124-192404_01."+suffix+".csv"));
id="Z-D_20261124-203450_02";
d=@(suffix) read(fullfile(results,'D',id,id+"."+suffix+".csv"));
k=1.2; colors=[0 .447 .741; .851 .325 .098; .15 .47 .34; .42 .28 .64; .35 .35 .35];
styles={'-','--','-.',':','-'};
pos='位置（mm）'; center='双向中心电流（mA）';
T=bc("references");
N=sortrows(read(fullfile(root,'models','position_B_primary_v1.nodes.csv')),'position_pulse');
B=T(T.phase=="B",:); x=N.position_pulse/1000; runs=unique(B.group,'stable');
for j=1:2
    rows=sortrows(B(B.group==runs(j),:),'position');
    assert(isequal(round(rows.position*1000),N.position_pulse));
    curves(:,j)=rows.ip; positive(:,j)=rows.positive; negative(:,j)=rows.negative;
end

assert(max(abs(mean(curves,2)-N.current_raw))<1e-9);
A=read(fullfile(root,'data','raw','Z-A1_20261124-144034_01.csv'));
A2=read(fullfile(results,'Z-A2_20261124-150201_03.windows.csv'));
S=read(fullfile(results,'Z-A2_20261124-150201_03.repeatability.csv'));
sites=[8 16 24]; mid=.5:29.5; h=nan(30,3);
for j=1:3
    z=A(A.stage=="hold" & A.target_mm==sites(j),:);
    t=z.t_s-z.t_s(1);
    h(:,j)=arrayfun(@(a) mean(z.current_fb_raw(z.valid==1 & t>=a & t<a+1)),0:29).';
end
f=canvas(colors,490); tiledlayout(1,2,'Padding','compact','TileSpacing','compact');
ax=nexttile;
for j=1:3, plot(mid,k*h(:,j),styles{j},'LineWidth',1.3); end
xlim([0 30]); xlabel('连续保持时间（s）'); ylabel('一秒平均电流（mA）');
ttl('(a) 连续保持'); holdLegend=lg(append(string(sites)," mm"),'northeast');
nexttile;
for j=1:2
    direction=3-2*j; offset=.26*direction;
    z=A2(A2.direction==direction,:);
    for site=sites
        a=z(z.position==site & isfinite(z.fourth_second_mean_raw),:);
        assert(height(a)==5);
        scatter(site+offset+linspace(-.12,.12,height(a)),k*a.fourth_second_mean_raw,13,colors(j,:), ...
            'filled','MarkerFaceAlpha',.42,'HandleVisibility','off');
    end
    q=sortrows(S(S.direction==direction,:),'position'); markers={'o','s'};
    handles(j)=errorbar(q.position+offset,k*q.mean_raw,k*q.repeat_std_raw,markers{j}, ...
        'LineWidth',1.1,'Color',colors(j,:),'MarkerFaceColor','w','CapSize',7);
end
xlim([5 27]); xticks(sites); xlabel('目标位置（mm）'); ylabel('第 4 秒平均电流（mA）');
ttl('(b) 重复到位'); legend(handles,{'正向，n = 5','反向，n = 5'},'Location','northwest','Box','off','FontName','SimSun');
drawnow; ax.Units='normalized'; b=ax.Position;
holdLegend.Units='normalized';
legendPosition=holdLegend.Position;
holdLegend.Position=[b(1)+.72*b(3), b(2)+.66*b(4), legendPosition(3:4)];
inset=axes(f,'Position',[b(1)+.43*b(3),b(2)+.20*b(4),.48*b(3),.30*b(4)],'FontSize',8);
for j=1:3, plot(mid(1:5),k*h(1:5,j),[styles{j} 'o'],'MarkerSize',3,'LineWidth',.8); end
xlim([0 5]); xticks(0:5); title('0–5 s 放大','FontName','SimSun','FontSize',9,'FontWeight','normal');
finish(f,out,1);

f=canvas(colors,475); plot(x,k*N.current_raw,'-','LineWidth',1.4);
plot(x,k*mean(curves,2),'.','Color',colors(5,:),'MarkerSize',5);
xlim([1 31]); labels(pos,center); lg({'冻结分段线性模型','静态建模节点'},'northwest'); finish(f,out,2);

f=canvas(colors,540); tiledlayout(2,1,'Padding','compact','TileSpacing','compact');
nexttile; plot(x,k*curves(:,1),'-','LineWidth',1.35);
plot(x,k*curves(:,2),'--','LineWidth',1.2); xlim([1 31]); ylabel(center);
ttl('(a) 两轮静态主网格'); lg({'第 1 轮','第 2 轮'},'northwest');
nexttile; plot(x,k*diff(curves,1,2),'LineWidth',1.15); zero(); xlim([1 31]);
labels(pos,'第二轮减第一轮（mA）'); ttl('(b) 逐位置差值'); finish(f,out,3);

f=canvas(colors,540); tiledlayout(2,1,'Padding','compact','TileSpacing','compact');
nexttile; plot(x,k*mean(positive,2),'-','LineWidth',1.15);
plot(x,k*mean(negative,2),'--','LineWidth',1.15);
plot(x,k*mean(curves,2),'-.','LineWidth',1.35);
xlim([1 31]); ylabel('空载电流（mA）'); ttl('(a) 到位方向与双向半和');
lg({'正向到位','反向到位','双向半和'},'northwest');
nexttile; plot(x,k*mean(positive-negative,2)/2,'LineWidth',1.15);
xlim([1 31]); labels(pos,'方向半差（mA）'); ttl('(b) 正向减反向的半差'); finish(f,out,4);

nodes=sortrows(bc("C_shared_nodes"),'position_mm'); q=nodes.position_mm;
cvLabels={'B 静态','C 2 mm/s','C 10 mm/s','C 40 mm/s'}; speeds=[2 10 40];
f=canvas(colors,485);
static=interp1(x,N.current_raw,q); comparisonRms=zeros(1,3);
plot(q,k*static,'-','LineWidth',1.5);
for j=1:3
    z=T(T.phase=="C" & T.speed==speeds(j),:);
    [g,xg]=findgroups(z.position); counts=splitapply(@numel,z.ip,g);
    y=splitapply(@mean,z.ip,g);
    [found,idx]=ismember(q,xg); assert(all(found) && all(counts(idx)==3));
    comparisonRms(j)=k*sqrt(mean((y(idx)-static).^2));
    plot(q,k*y(idx),styles{j+1},'LineWidth',1.25);
end
fprintf('Static versus constant speed RMSE (mA): %.5f %.5f %.5f\n',comparisonRms);
xlim(q([1 end])); labels(pos,center); lg(cvLabels,'northwest'); finish(f,out,5);

scores=bc("model_scores"); metrics=[scores.RMSE_raw,scores.P95_raw];
f=canvas(colors,430); tiledlayout(1,2,'Padding','compact','TileSpacing','compact');
modelNames={'常值','线性','二次','分段线性','PCHIP'}; marks={'o','s','^','d','x'};
for i=1:2
    nexttile;
    for j=1:5
        z=scores.model_1to5==j;
        plot((1:4)+(j-3)*.12,k*metrics(z,i),marks{j},'Color',colors(j,:), ...
            'MarkerSize',5.5,'LineWidth',1.15);
    end
    xticks(1:4); xticklabels(cvLabels); xtickangle(18); xlim([.55 4.45]);
    set(gca,'YScale','log','TickLabelInterpreter','none','FontSize',10);
    ylim([0.85*k*min(metrics(:)) 1.15*k*max(metrics(:))]);
    if i==1, ttl('(a) 均方根误差'); else, ttl('(b) 绝对误差 P95'); end
    ylabel('电流误差（mA，对数刻度）');
    if i==1, cvLegend=lg(modelNames,'southoutside'); end
end
cvLegend.NumColumns=5; cvLegend.Layout.Tile='south';
finish(f,out,6);

P=d("center_predictions"); M=d("full_metrics"); e=k*P.error_B_raw;
limits=ceil(max(abs(e),[],'omitnan')/5)*5*[-1 1];
f=canvas(colors,645); tiledlayout(2,2,'Padding','compact','TileSpacing','compact');
for j=1:3
    if j==3, nexttile([1 2]); else, nexttile; end
    phase="D"+j; z=P(P.stage==phase & isfinite(P.error_B_raw),:);
    s=unique(z.speed_pulse_s);
    for i=1:numel(s)
        zz=z(z.speed_pulse_s==s(i),:); m=M(M.stage==phase & M.speed_pulse_s==s(i) & M.scope=="all",:);
        assert(height(m)==1 && height(zz)==m.n);
        assert(abs(sqrt(mean(zz.error_B_raw.^2))-m.rmse_raw)<1e-9);
        plot(zz.position_pulse/1000,k*zz.error_B_raw,marks{i},'Color',colors(i,:), ...
            'LineStyle','none','MarkerSize',3.8,'LineWidth',.65);
    end
    xlim([1 31]); ylim(limits); zero(); labels(pos,'实测减预测（mA）');
    titles={'(a) D1 未见位置','(b) D2 同日另时段','(c) D3 恒速留出'}; ttl(titles{j});
    if j==3, lg(append(string(s/1000)," mm/s"),'northwest'); end
end
finish(f,out,7);

W=d("direction_metrics"); R=d("direction_residuals"); f=canvas(colors,430);
tiledlayout(1,2,'Padding','compact','TileSpacing','compact');
W=W(W.model=="B" & W.scope=="all",:); cap=ceil(k*max(abs(W.mean_error_raw))/10)*10;
for j=1:2
    nexttile;
    for i=1:2
        z=sortrows(W(W.stage=="D"+j & W.direction==3-2*i,:),'window');
        r=R(R.stage=="D"+j & R.direction==3-2*i,:);
        for w=1:height(z)
            values=r.error_B_raw(r.window==z.window(w) & isfinite(r.error_B_raw));
            assert(numel(values)==z.n(w) && abs(mean(values)-z.mean_error_raw(w))<1e-9);
        end
        plot(1:4,k*z.mean_error_raw,[styles{i} marks{i}],'LineWidth',1.3,'MarkerSize',5);
    end
    xlim([.8 4.2]); ylim([-cap cap]); xticks(1:4); zero();
    labels('保持窗口（s）','方向平均残差（mA）');
    titles={'(a) D1 未见位置','(b) D2 同日另时段'}; ttl(titles{j});
    lg({'正向到位','反向到位'},'southwest');
end
finish(f,out,8);
end

function t=read(filename)
t=readtable(filename,'TextType','string');
end

function f=canvas(colors,height)
f=figure('Visible','off','Color','w','Position',[80 70 960 height], ...
    'DefaultAxesFontName','Times New Roman','DefaultAxesFontSize',12, ...
    'DefaultTextFontName','SimSun','DefaultAxesColor','w', ...
    'DefaultAxesBox','on','DefaultAxesTickDir','in','DefaultAxesXGrid','on', ...
    'DefaultAxesYGrid','on','DefaultAxesGridAlpha',.16,'DefaultAxesLineWidth',.7, ...
    'DefaultAxesNextPlot','add','DefaultAxesColorOrder',colors);
end

function labels(x,y)
xlabel(x,'FontName','SimSun'); ylabel(y,'FontName','SimSun');
end

function ttl(label)
title(label,'FontName','SimSun','FontWeight','normal');
end

function h=lg(items,placement)
h=legend(items,'Location',placement,'Box','off','FontName','SimSun','FontSize',10);
end

function zero()
yline(0,':','Color',[.35 .35 .35],'HandleVisibility','off');
end

function finish(f,out,n)
stem=fullfile(out,sprintf('position_%02d',n));
savefig(f,[stem '.fig']);
exportgraphics(f,[stem '.svg'],'ContentType','vector');
exportgraphics(f,[stem '.png'],'Resolution',400);
close(f);
end
