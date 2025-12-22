import org.jenkinsci.plugins.workflow.steps.FlowInterruptedException

pipeline {
	agent { label 'rs-orin-01.realsenseai.com' }
	options {
		timestamps()
			timeout(time: 10, unit: 'MINUTES')
	}

	stages {
		stage('pytest') {
			steps {
				script {
					sh 'pytest --tb=no -s test'
				}
			}
		}
	}
}
