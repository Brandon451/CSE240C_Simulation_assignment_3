import numpy as np
import matplotlib.pyplot as plt
import pandas as pd
import seaborn as sns

pd.set_option("display.max_rows", None, "display.max_columns", None)

sns.set_style('darkgrid')

#df = pd.read_csv("ipc_collected.csv", usecols = ["Tracename", "Reset: 4096", "Reset: 16384", "Distahead: 4", "Distahead: 8", "Distahead: 12", "Distahead: 40"], nrows = 50)
#df = pd.read_csv("ipc_collected.csv", usecols = ["Tracename", "16k", "32k", "128k", "256k"], nrows = 50)
df = pd.read_csv("results_compiled.csv", usecols = ["Tracename", "16k", "32k", "128k", "256k"], nrows = 50)
#df = pd.read_csv("results_compiled.csv", usecols = ["Tracename", "Reset: 4096", "Reset: 16384", "Distahead: 4", "Distahead: 8", "Distahead: 12", "Distahead: 40"], nrows = 50)
#df = pd.melt(df, id_vars = "Tracename", var_name = "Policy", value_name = "% Speed Up")
#df = pd.melt(df, id_vars = "Tracename", var_name = "Policy", value_name = "MPKI")
df = pd.melt(df, id_vars = "Tracename", var_name = "Policy", value_name = "% Change in MPKI")
print(df)
#df["% Speed Up"] = df["% Speed Up"].astype(float)
#df["MPKI"] = df["MPKI"].astype(float)
df["% Change in MPKI"] = df["% Change in MPKI"].astype(int)

#x_axis = df['Tracename'].tolist()[0:51]

#print(x_axis)
#print(type(x_axis))
#print(len(x_axis))

#y_axis1 = df['DJOLT_base_improvement_relative'].tolist()[0:51]

#print(y_axis1)
#print(type(y_axis1))
#print(len(y_axis1))

#sns.lineplot(x = x_axis, y = y_axis_IPC, color = 'orange')
#ax2 = plt.twinx()
#sns.lineplot(x = x_axis, y = y_axis_MPKI, ax=ax2, color = 'blue')


#sns.barplot(x_axis, y_axis1)
sns.set(font_scale = 1.2)
#sns.catplot(x = "Tracename", y="% Speed Up", hue = "Policy", data = df, kind = "bar", height=5, aspect=3.8, legend=False).set(title = "Baseline DJOLT and FNL+MMA IPC speedup over LRU")
#sns.catplot(x = "Tracename", y="% Decrease in MPKI", hue = "Policy", data = df, kind = "bar", height=5, aspect=3.8, legend=False).set(title = "Baseline DJOLT and FNL+MMA %MPKI decrease over LRU")
#sns.boxplot(x = "Policy", y = "% Speed Up", data = df).set(title = "%Speed up over baseline FNLMMA for hardware budget change")
sns.boxplot(x = "Policy", y = "% Change in MPKI", data = df).set(title = "%MPKI change for hardware budget exploration")
plt.xticks(rotation = 35, ha="right", rotation_mode="anchor")
plt.legend(loc='upper right')
plt.tight_layout()
plt.show()
