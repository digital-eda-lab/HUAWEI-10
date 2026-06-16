## 测试表格
```
https://aws128smuu4.feishu.cn/wiki/LQZ3woILLiqsxXk4cgRcLOiUnAg
```
## 评分脚本
```
python score_rank.py a.csv b.csv c.csv

其中csv文件格式如下
casename,and_count,lev_count,runtime_sec,extra_peak_rss_mb
tc_public_1,97,9,0.151320,1.320
```
## 批量运行脚本
```
python run_tc_public_abc.py --flow /path/to/flow/script --cases 1-30(也可以1，2，3，5-20写法) --output a.csv
```
