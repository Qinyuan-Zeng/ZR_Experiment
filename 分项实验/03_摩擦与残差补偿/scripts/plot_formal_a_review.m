function plot_formal_a_review
root=fileparts(fileparts(mfilename('fullpath')));
folder=fullfile(root,'results','review','Z-AF-20261126-03');
t=readtable(fullfile(folder,'space.csv'),TextType="string");
e=readtable(fullfile(folder,'errors.csv'),TextType="string");
f=figure(Visible="off",Color="white",Position=[60 60 1120 680]);
clean=onCleanup(@()close(f));
tiledlayout(f,2,2,Padding="compact",TileSpacing="compact");
for component=["half_diff","half_sum"]
    a=nexttile; hold(a,"on"); grid(a,"on"); box(a,"on");
    for v=[2000 10000 40000]
        rows=t(t.component==component & t.speed==v,:);
        plot(a,rows.position,1.2*rows.S02_centered,LineWidth=1.2,DisplayName=string(v)+" 脉冲/s");
    end
    if component=="half_diff", name="半差"; else, name="半和"; end
    title(a,"独立会话的"+name+"空间分布",FontName="SimSun");
    xlabel(a,"位置（脉冲）",FontName="SimSun"); ylabel(a,"相对该档位置均值的偏差（mA）",FontName="SimSun");
    legend(a,Location="best",FontName="SimSun",Box="off");
end
for d=[1 -1]
    a=nexttile;hold(a,"on");grid(a,"on");box(a,"on");
    for m=["F0","F1"]
        z=e(e.direction==d & e.split=="S02" & e.model==m,:);
        style='--o'; if m=="F1", style='-s'; end
        plot(a,z.speed,1.2*z.rmse,style,LineWidth=1.2,DisplayName=m);
    end
    if d==1, name="正向"; else, name="反向"; end
    title(a,"独立会话"+name+"预测误差",FontName="SimSun");
    xlabel(a,"速度绝对值（脉冲/s）",FontName="SimSun");ylabel(a,"均方根误差（mA）",FontName="SimSun");
    ylim(a,[0 18]);legend(a,Location="best",Box="off");
end
set(findall(f,Type="axes"),FontName="Times New Roman",TickDir="in");
base=fullfile(folder,'spatial_and_boundary');
savefig(f,[base '.fig']);exportgraphics(f,[base '.svg'],ContentType="vector");
exportgraphics(f,[base '.png'],Resolution=400);
end
