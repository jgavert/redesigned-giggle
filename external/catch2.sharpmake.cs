using Sharpmake;

[module: Sharpmake.Include("../sharpmake/common_project.sharpmake.cs")]

namespace externals
{
    [Sharpmake.Generate]
    class Catch2 : CoroutineStealer.CommonProject
    {
        public string BasePath = @"[project.ProjectBasePath]/external/Catch2";

        public Catch2()
        {
            Name = "Catch2";
            SourceRootPath = "[project.BasePath]/single_include";
            SourceFilesBlobExcludeRegex.Add("[project.BasePath]/include/*");
            SourceFiles.Add("[project.BasePath]/single_include/catch2/catch.hpp");
            
        }

        [Configure()]
        public void Configure(Project.Configuration conf, Target target)
        {
            base.ConfigureAll(conf, target);
            conf.Output = Project.Configuration.OutputType.None;
            conf.AdditionalCompilerOptions.Add("/wd4819");
            
            conf.ExportDefines.Add("_HAS_DEPRECATED_RESULT_OF");
            conf.IncludePaths.Add("[project.BasePath]/single_include");
            conf.TargetLibraryPath = "[project.BasePath]/lib/[target.OutputType]";
            conf.SolutionFolder = "external";
        }
    }
}
