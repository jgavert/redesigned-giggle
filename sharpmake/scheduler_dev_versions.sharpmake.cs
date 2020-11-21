using Sharpmake;

[module: Sharpmake.Include("common_project.sharpmake.cs")]

namespace CoroutineStealer
{
    [Sharpmake.Generate]
    class SchedulerDevVersions : CommonProject
    {
        public string BasePath = @"[project.ProjectBasePath]/SchedulerDevVersions";

        public SchedulerDevVersions()
        {
            Name = "SchedulerDevVersions";
            SourceRootPath = "[project.BasePath]/src";
        }

        [Configure()]
        public void Configure(Project.Configuration conf, Target target)
        {
            base.ConfigureAll(conf, target);
            conf.Output = Project.Configuration.OutputType.Lib;
            conf.TargetLibraryPath = "[project.BasePath]/lib/[target.OutputType]";
            conf.SolutionFolder = "lib";
        }
    }
}
